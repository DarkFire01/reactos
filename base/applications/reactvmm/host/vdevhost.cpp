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

    *Object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP LegacyPortAdapter::NotifyIoPortRead(USHORT Port, ULONG Width,
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

STDMETHODIMP LegacyPortAdapter::NotifyIoPortWrite(USHORT Port, ULONG Width,
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

STDMETHODIMP EmulationServices::RegisterIoPortHandler(USHORT FirstPort,
                                                      USHORT LastPort,
                                                      ULONG Widths,
                                                      IVndIoPortHandler *Handler,
                                                      ULONG Flags,
                                                      void **Registration)
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

    /*
     * What comes back is only ever handed straight back to give the range up,
     * so the handler itself is as good a token as any and needs nothing kept.
     */
    if (Registration != nullptr)
        *Registration = Handler;

    return S_OK;
}

STDMETHODIMP EmulationServices::RegisterGpaRange(ULONG64 FirstPage,
                                                 ULONG64 PageCount,
                                                 void *Handler, BOOL Enabled,
                                                 void **Registration)
{
    UNREFERENCED_PARAMETER(Enabled);

    if ((Handler == nullptr) || (PageCount == 0))
        return E_INVALIDARG;

    Log(RtvmLogWarning,
        "a device wants the %llu page(s) at %llx, which is not answered yet\n",
        PageCount, FirstPage * 0x1000);

    if (Registration != nullptr)
        *Registration = nullptr;

    return E_NOTIMPL;
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

STDMETHODIMP ProcessorServices::SetPendingInterrupt(ULONG64 Kind,
                                                    ULONG64 Reserved,
                                                    ULONG Vector)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Kind != VDEV_INTERRUPT_FROM_PIC)
        return E_NOTIMPL;

    m_Owner.Owner().OfferVector(Vector);
    return S_OK;
}

STDMETHODIMP ProcessorServices::TakePendingInterrupt()
{
    /*
     * Failing is what says it has not been taken. A controller tests this for
     * success rather than against a particular code, so anything that is not a
     * failure would have it put a vector in service that never went anywhere.
     */
    return m_Owner.Owner().VectorWasTaken() ? S_OK : E_PENDING;
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
    : m_Machine(Owner), m_Emulation(*this), m_Processors(*this), m_Services(*this)
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
bool VdevHost::Create(REFCLSID Class, const char *Name)
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
     * Handed the manager itself, which is where it goes looking for the
     * services it named. Its configuration is nothing yet.
     */
    HRESULT Status = Device->Initialize(nullptr, 0, static_cast<IUnknown *>(this));

    if (FAILED(Status))
    {
        Log(RtvmLogError, "%s would not initialise, %08lx\n", Name, Status);
        Device->Release();
        return false;
    }

    Status = Device->StartReservingResources();

    if (SUCCEEDED(Status))
        Status = Device->FinishReservingResources();

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
        const HRESULT Status = Kept.Device->PowerOnCold();

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
        m_Vdevs[Index - 1].Device->PowerOff();
}

void VdevHost::ResetAll()
{
    for (const LoadedVdev &Kept : m_Vdevs)
        Kept.Device->Reset();

    for (const LoadedVdev &Kept : m_Vdevs)
        Kept.Device->PostReset();
}

} /* namespace rtvm */
