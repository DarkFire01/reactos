/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The manager's end of the virtual device contract
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "vdevhost.h"

#include <stdio.h>
#include <string.h>

namespace rtvm
{

/* THE OLDER DEVICES, MADE TO LOOK LIKE THE NEWER ONES ************************/

STDMETHODIMP LegacyPortAdapter::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVndIoPortHandler))
    {
        *Object = static_cast<IVndIoPortHandler *>(this);
        AddRef();
        return S_OK;
    }

    if (IsEqualIID(Interface, IID_IVndMmioHandler))
    {
        *Object = static_cast<IVndMmioHandler *>(this);
        AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP LegacyPortAdapter::NotifyMmioRead(ULONG64 Address, ULONG Length,
                                               void *Buffer)
{
    if ((m_Device->Vtable == nullptr) || (m_Device->Vtable->MemoryRead == nullptr))
        return E_NOTIMPL;

    return (m_Device->Vtable->MemoryRead(m_Device, Address, Length, Buffer) == RtvmOk)
         ? S_OK
         : E_FAIL;
}

STDMETHODIMP LegacyPortAdapter::NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                                const void *Buffer)
{
    if ((m_Device->Vtable == nullptr) || (m_Device->Vtable->MemoryWrite == nullptr))
        return E_NOTIMPL;

    return (m_Device->Vtable->MemoryWrite(m_Device, Address, Length, Buffer) == RtvmOk)
         ? S_OK
         : E_FAIL;
}

STDMETHODIMP LegacyPortAdapter::NotifyIoPortRead(USHORT Port, USHORT Width,
                                                 ULONG *Value)
{
    if (Value == nullptr)
        return E_POINTER;

    *Value = Bus::Floating;

    if ((m_Device->Vtable == nullptr) || (m_Device->Vtable->IoRead == nullptr))
        return E_NOTIMPL;

    if (m_Device->Vtable->IoRead(m_Device, Port, Width, Value) != RtvmOk)
    {
        *Value = Bus::Floating;
        return E_FAIL;
    }

    return S_OK;
}

STDMETHODIMP LegacyPortAdapter::NotifyIoPortWrite(USHORT Port, USHORT Width,
                                                  ULONG Value)
{
    if ((m_Device->Vtable == nullptr) || (m_Device->Vtable->IoWrite == nullptr))
        return E_NOTIMPL;

    return (m_Device->Vtable->IoWrite(m_Device, Port, Width, Value) == RtvmOk)
         ? S_OK
         : E_FAIL;
}

/* WHERE A DEVICE ASKS FOR ITS ADDRESSES **************************************/

STDMETHODIMP EmulationServices::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVmAmd64EmulationServices))
    {
        *Object = static_cast<IVmAmd64EmulationServices *>(this);
        AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

/* WHAT A RESERVATION IS ******************************************************/

STDMETHODIMP Reservation::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown))
    {
        *Object = static_cast<IVndRegistration *>(this);
        AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Reservation::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_Count);
}

STDMETHODIMP_(ULONG) Reservation::Release()
{
    const ULONG Left = (ULONG)InterlockedDecrement(&m_Count);

    if (Left == 0)
        delete this;

    return Left;
}

/*
 * Giving the range back. A device that is switched off and on again reserves
 * afresh, so nothing is kept here to reserve again with.
 */
STDMETHODIMP Reservation::Revoke()
{
    if (m_Ports != nullptr)
    {
        m_Bus.ForgetPorts(m_Ports);
        m_Ports = nullptr;
    }

    if (m_Memory != nullptr)
    {
        m_Bus.ForgetMemory(m_Memory);
        m_Memory = nullptr;
    }

    return S_OK;
}

/* WHERE A DEVICE ASKS FOR ONE ************************************************/

STDMETHODIMP EmulationServices::RegisterIoPortHandler(USHORT FirstPort,
                                                      USHORT LastPort,
                                                      ULONG Widths,
                                                      IVndIoPortHandler *Handler,
                                                      ULONG Flags,
                                                      IVndRegistration **Registration)
{
    UNREFERENCED_PARAMETER(Widths);
    UNREFERENCED_PARAMETER(Flags);

    if ((Handler == nullptr) || (LastPort < FirstPort))
        return E_INVALIDARG;

    const ULONG Count = (ULONG)(LastPort - FirstPort) + 1;

    if (!m_Owner.Owner().SystemBus().ClaimPorts(Handler, FirstPort,
                                                (USHORT)Count))
    {
        Log(RtvmLogError, "ports %04x to %04x are answered for already\n",
            FirstPort, LastPort);
        return E_ACCESSDENIED;
    }

    Log(RtvmLogTrace, "ports %04x to %04x taken\n", FirstPort, LastPort);

    if (Registration != nullptr)
    {
        *Registration = new Reservation(m_Owner.Owner().SystemBus(), Handler);

        if (*Registration == nullptr)
        {
            m_Owner.Owner().SystemBus().ForgetPorts(Handler);
            return E_OUTOFMEMORY;
        }
    }

    return S_OK;
}

STDMETHODIMP EmulationServices::RegisterMmioHandler(ULONG64 FirstPage,
                                                    ULONG64 PageCount,
                                                    IVndMmioHandler *Handler,
                                                    BOOL Enabled,
                                                    IVndRegistration **Registration)
{
    UNREFERENCED_PARAMETER(Enabled);

    if ((Handler == nullptr) || (PageCount == 0))
        return E_INVALIDARG;

    const ULONG64 Base = FirstPage * VDEV_PAGE_SIZE;
    const ULONG64 Length = PageCount * VDEV_PAGE_SIZE;

    if (!m_Owner.Owner().SystemBus().ClaimMemory(Handler, Base, Length))
    {
        Log(RtvmLogError, "%llx to %llx is answered for already\n",
            Base, Base + Length - 1);
        return E_ACCESSDENIED;
    }

    Log(RtvmLogTrace, "%llu page(s) at %llx taken\n", PageCount, Base);

    /*
     * The window is claimed before the machine decides what to map, so nothing
     * has to be taken back out of the guest afterwards. A device that reserves
     * one once the machine is running is asking for a page the guest already
     * has, and is refused.
     */
    if (m_Owner.Owner().Running())
    {
        m_Owner.Owner().SystemBus().ForgetMemory(Handler);
        Log(RtvmLogError, "%llx is already the guest's, so it cannot be taken\n",
            Base);
        return E_NOT_VALID_STATE;
    }

    if (Registration != nullptr)
    {
        *Registration = new Reservation(m_Owner.Owner().SystemBus(), Handler);

        if (*Registration == nullptr)
        {
            m_Owner.Owner().SystemBus().ForgetMemory(Handler);
            return E_OUTOFMEMORY;
        }
    }

    return S_OK;
}

/* THE PROCESSORS, AS A DEVICE SEES THEM **************************************/

STDMETHODIMP ProcessorServices::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVmProcessorServices))
    {
        *Object = static_cast<IVmProcessorServices *>(this);
        AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP ProcessorServices::GetVirtualProcessorCount(ULONG *Count)
{
    if (Count == nullptr)
        return E_POINTER;

    *Count = m_Owner.Owner().ProcessorCount();
    return S_OK;
}

STDMETHODIMP ProcessorServices::AssertVirtualProcessorInterrupt(ULONG64 Delivery,
                                                                ULONG64 Reserved,
                                                                ULONG Vector)
{
    /*
     * The old pair of chips has no say in where an interrupt goes: it holds
     * one up and whichever processor is listening takes it. Everything since
     * says which processor and how, and that goes to the controller each
     * processor has of its own rather than through the line the pair share.
     */
    if (Delivery == VDEV_DELIVERY_EXTERNAL)
    {
        m_Owner.Owner().OfferVector(Vector);
        return S_OK;
    }

    /*
     * The two that name a vector. Which processor takes one of the second
     * kind is the controllers' business between them, and saying which is
     * what a router does when it has no reason to prefer one.
     */
    if ((Delivery != VDEV_DELIVERY_FIXED) && (Delivery != VDEV_DELIVERY_LOWEST))
        return E_NOTIMPL;

    const ULONG Where = (ULONG)(Reserved & 0xFF);
    const bool Lowest = (Delivery == VDEV_DELIVERY_LOWEST);
    const bool Logical = (Reserved & IOAPIC_SAID_LOGICAL) != 0;
    const bool Level = (Reserved & IOAPIC_SAID_LEVEL) != 0;

    return m_Owner.Owner().RequestVector(Where, Vector, Lowest, Logical, Level)
         ? S_OK
         : E_NOTIMPL;
}

STDMETHODIMP ProcessorServices::ClearVirtualProcessorInterrupt()
{
    /*
     * Failing is what says it has not been taken. A controller tests this for
     * success rather than against a particular code, so anything that is not a
     * failure would have it put a vector in service that never went anywhere.
     */
    return m_Owner.Owner().VectorWasTaken() ? S_OK : E_PENDING;
}

/* THE GUEST'S MEMORY, AS A DEVICE SEES IT ************************************/

STDMETHODIMP GuestMemoryAccess::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVmGuestMemoryAccess))
    {
        *Object = static_cast<IVmGuestMemoryAccess *>(this);
        AddRef();
        return S_OK;
    }

    if (IsEqualIID(Interface, IID_IRtvmApertureServices))
    {
        *Object = static_cast<IRtvmApertureServices *>(this);
        AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP GuestMemoryAccess::CreateAperture(ULONG64 Base, ULONG64 Length,
                                               void **Where)
{
    if (Where == nullptr)
        return E_POINTER;

    return m_Owner.Owner().CreateAperture(Base, Length, Where) ? S_OK
                                                               : E_FAIL;
}

STDMETHODIMP GuestMemoryAccess::ReadRamBytes(ULONG64 Address, void *Buffer,
                                             ULONG Length)
{
    if (Buffer == nullptr)
        return E_POINTER;

    return m_Owner.Owner().ReadGuest(Address, Buffer, Length) ? S_OK : E_BOUNDS;
}

STDMETHODIMP GuestMemoryAccess::WriteRamBytes(ULONG64 Address,
                                              const void *Buffer, ULONG Length)
{
    if (Buffer == nullptr)
        return E_POINTER;

    return m_Owner.Owner().WriteGuest(Address, Buffer, Length) ? S_OK : E_BOUNDS;
}

STDMETHODIMP GuestMemoryAccess::TranslateGvaToGpa(ULONG64 Address,
                                                  ULONG64 *Physical)
{
    if (Physical == nullptr)
        return E_POINTER;

    WHV_TRANSLATE_GVA_RESULT_CODE Result = WHvTranslateGvaResultSuccess;

    /* The first processor's, because nothing here has asked about another */
    return m_Owner.Owner().Translate(0, Address, 0, &Result, Physical);
}

/* HOW A DEVICE REACHES EVERYTHING ELSE ***************************************/

STDMETHODIMP ServiceAccess::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVmServiceAccess))
    {
        *Object = static_cast<IVmServiceAccess *>(this);
        AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP ServiceAccess::GetService(REFIID Service, void **Object)
{
    return m_Owner.FindService(Service, Object);
}

/* THE HOST *******************************************************************/

VdevHost::VdevHost(Machine &Owner)
    : m_Machine(Owner), m_Emulation(*this), m_Processors(*this),
      m_Memory(*this), m_Services(*this)
{
}

VdevHost::~VdevHost()
{
    PowerOffAll();

    for (ULONG Index = m_Vdevs.Count(); Index > 0; Index--)
    {
        IVirtualDevice *Device = m_Vdevs[Index - 1].Device;

        Device->Teardown();
        Device->Release();
    }

    m_Vdevs.Clear();

    /*
     * The libraries stay loaded. Every device in them is gone, but a library is
     * free to have left a thread behind, and unloading under one of those costs
     * more than the handle does.
     */
    m_Libraries.Clear();
}

STDMETHODIMP VdevHost::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown))
    {
        *Object = static_cast<IUnknown *>(this);
        AddRef();
        return S_OK;
    }

    if (IsEqualIID(Interface, IID_IVmServiceAccess))
    {
        *Object = static_cast<IVmServiceAccess *>(&m_Services);
        m_Services.AddRef();
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

HRESULT VdevHost::FindService(REFIID Service, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    *Object = nullptr;

    if (IsEqualIID(Service, IID_IVmAmd64EmulationServices))
    {
        *Object = static_cast<IVmAmd64EmulationServices *>(&m_Emulation);
        m_Emulation.AddRef();
        return S_OK;
    }

    if (IsEqualIID(Service, IID_IVmProcessorServices))
    {
        *Object = static_cast<IVmProcessorServices *>(&m_Processors);
        m_Processors.AddRef();
        return S_OK;
    }

    /* Whoever is looking at the machine, which is not a device at all */
    if (IsEqualIID(Service, IID_IMonitorDevice) &&
        (m_Machine.Monitor() != nullptr))
    {
        return m_Machine.Monitor()->QueryInterface(Service, Object);
    }

    if (IsEqualIID(Service, IID_IVmGuestMemoryAccess))
    {
        *Object = static_cast<IVmGuestMemoryAccess *>(&m_Memory);
        m_Memory.AddRef();
        return S_OK;
    }

    if (IsEqualIID(Service, IID_IRtvmApertureServices))
    {
        *Object = static_cast<IRtvmApertureServices *>(&m_Memory);
        m_Memory.AddRef();
        return S_OK;
    }

    /* One device asking for another, which only works once that one is up */
    for (ULONG Index = 0; Index < m_Published.Count(); Index++)
    {
        if (IsEqualIID(Service, m_PublishedAs[Index]))
            return m_Published[Index]->QueryInterface(Service, Object);
    }

    Log(RtvmLogWarning,
        "a device wants {%08lx-%04x-%04x-...}, which nothing here offers\n",
        Service.Data1, Service.Data2, Service.Data3);

    return E_NOINTERFACE;
}

bool VdevHost::Load(const char *FileName)
{
    if (m_Libraries.Full())
    {
        Log(RtvmLogError, "no room for another device library\n");
        return false;
    }

    UniqueLibrary Library(LoadLibraryA(FileName));

    if (!Library)
    {
        Log(RtvmLogError, "%s would not load, error %lu\n", FileName, GetLastError());
        return false;
    }

    /*
     * The one export reached by name. Without it the file is a library of some
     * other kind and nothing here can ask it for anything.
     */
    if (GetProcAddress(Library.Get(), "DllGetClassObject") == nullptr)
    {
        Log(RtvmLogError, "%s hands out no class objects\n", FileName);
        return false;
    }

    Log(RtvmLogTrace, "%s is a device library\n", FileName);
    return m_Libraries.Add(Library.Release());
}

/**
 * @brief
 * Makes one device of the named kind and brings it as far as being ready to run.
 *
 * @remarks
 * Every loaded library is asked in turn and the first that has the kind makes
 * it. That is what the class identifier is for: which library a kind came out
 * of is not the manager's business and does not appear anywhere it decides
 * anything.
 */
bool VdevHost::Create(REFCLSID Class, const char *Name,
                      const char *Settings)
{
    if (m_Vdevs.Full())
    {
        Log(RtvmLogError, "no room for another device\n");
        return false;
    }

    IVirtualDevice *Device = nullptr;

    for (HMODULE Library : m_Libraries)
    {
        auto GetClassObject = reinterpret_cast<HRESULT (STDAPICALLTYPE *)(
            REFCLSID, REFIID, void **)>(
            reinterpret_cast<void *>(
                GetProcAddress(Library, "DllGetClassObject")));

        if (GetClassObject == nullptr)
            continue;

        IClassFactory *Factory = nullptr;

        if (FAILED(GetClassObject(Class, IID_IClassFactory,
                                  reinterpret_cast<void **>(&Factory))))
        {
            continue;
        }

        const HRESULT Made = Factory->CreateInstance(nullptr, IID_IVirtualDevice,
                                                     reinterpret_cast<void **>(&Device));
        Factory->Release();

        if (FAILED(Made))
        {
            Log(RtvmLogError, "%s would not make a %s, %08lx\n",
                Name, Name, Made);
            return false;
        }

        break;
    }

    if (Device == nullptr)
    {
        Log(RtvmLogError, "no loaded library offers a %s\n", Name);
        return false;
    }

    /*
     * Told what to be before it is initialised, because what it depends on
     * and what addresses it wants both follow from that.
     */
    if (Settings != nullptr)
    {
        IRtvmDeviceSettings *Told = nullptr;

        if (SUCCEEDED(Device->QueryInterface(IID_IRtvmDeviceSettings,
                                             reinterpret_cast<void **>(&Told))))
        {
            const HRESULT Taken = Told->SetSettings(Settings);

            Told->Release();

            if (FAILED(Taken))
            {
                Log(RtvmLogError, "%s would not take %s, %08lx\n",
                    Name, Settings, Taken);
                Device->Release();
                return false;
            }
        }
    }

    /*
     * Handed the manager itself, which is where it goes looking for the
     * services it named.
     */
    HRESULT Status = Device->Initialize(nullptr, 0, static_cast<IUnknown *>(this));

    if (FAILED(Status))
    {
        Log(RtvmLogError, "%s would not initialise, %08lx\n", Name, Status);
        Device->Release();
        return false;
    }

    Status = Device->StartReservingResources(nullptr, VDEV_STATE_NONE);

    if (SUCCEEDED(Status))
        Status = Device->FinishReservingResources(VDEV_STATE_NONE);

    if (FAILED(Status))
    {
        Log(RtvmLogError, "%s could not have what it asked for, %08lx\n",
            Name, Status);
        Device->Teardown();
        Device->Release();
        return false;
    }

    /* Whatever it offers other devices, so that they can be given it later */
    static const IID *const Offered[] =
    {
        &IID_IVmPicService,
        &IID_IVmDmaController,
        &IID_IVmIoApic,
        &IID_IVideoVdev,
        &IID_IVmPitService,
        &IID_IVmPciBusService,
        &IID_IVmSuperIo,
        &IID_IVmInputController
    };

    for (const IID *Which : Offered)
    {
        IUnknown *Published = nullptr;

        if (SUCCEEDED(Device->QueryInterface(*Which,
                                             reinterpret_cast<void **>(&Published))))
        {
            m_Published.Add(Published);
            m_PublishedAs.Add(*Which);

            /* The ones everything else needs to be able to reach */
            if (IsEqualIID(*Which, IID_IVmPicService))
                m_Interrupts = static_cast<IVmPicService *>(Published);
            else if (IsEqualIID(*Which, IID_IVmDmaController))
                m_Transfers = static_cast<IVmDmaController *>(Published);
            else if (IsEqualIID(*Which, IID_IVmIoApic))
                m_Lines = static_cast<IVmIoApic *>(Published);
            else if (IsEqualIID(*Which, IID_IVideoVdev))
                m_Screen = static_cast<IVideoVdev *>(Published);
        }
    }

    LoadedVdev Kept;

    Kept.Device = Device;
    strncpy(Kept.Name, Name, sizeof(Kept.Name) - 1);
    Kept.Name[sizeof(Kept.Name) - 1] = '\0';

    Log(RtvmLogInfo, "%s is up\n", Name);
    return m_Vdevs.Add(Kept);
}

bool VdevHost::PowerOnAll()
{
    for (const LoadedVdev &Kept : m_Vdevs)
    {
        const HRESULT Status = Kept.Device->PowerOnCold(VDEV_STATE_NONE);

        if (FAILED(Status))
        {
            Log(RtvmLogError, "%s would not come up, %08lx\n", Kept.Name, Status);
            return false;
        }
    }

    return true;
}

void VdevHost::PowerOffAll()
{
    /* Backwards, so that a device goes before whatever it leans on */
    for (ULONG Index = m_Vdevs.Count(); Index > 0; Index--)
        m_Vdevs[Index - 1].Device->PowerOff(VDEV_STATE_NONE);
}

void VdevHost::ResetAll()
{
    for (const LoadedVdev &Kept : m_Vdevs)
        Kept.Device->Reset(VDEV_STATE_NONE);

    for (const LoadedVdev &Kept : m_Vdevs)
        Kept.Device->PostReset(VDEV_STATE_NONE);
}

} /* namespace rtvm */
