/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What every device in this library has in common
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "vdevbase.h"

namespace rtvm
{

volatile LONG g_Outstanding = 0;

/**
 * @brief
 * Hands back the services a device wants, with the ones it needs first.
 *
 * @param Spare
 * How many of the trailing entries the device will come up without. The caller
 * puts those last, which is the order the answer has to be in.
 */
HRESULT PublishDependencies(const GUID *const *Wanted, ULONG Count, ULONG Spare,
                            ULONG *Answered, GUID **Services, ULONG *Required)
{
    if ((Answered == nullptr) || (Services == nullptr) || (Required == nullptr))
        return E_POINTER;

    *Answered = 0;
    *Services = nullptr;
    *Required = 0;

    if (Spare > Count)
        return E_INVALIDARG;

    /*
     * The manager frees this, so it has to come from the allocator both ends
     * agree on rather than from whatever this library happens to use.
     */
    auto *List = static_cast<GUID *>(CoTaskMemAlloc(Count * sizeof(GUID)));

    if (List == nullptr)
        return E_OUTOFMEMORY;

    for (ULONG Index = 0; Index < Count; Index++)
        List[Index] = *Wanted[Index];

    *Services = List;
    *Answered = Count;
    *Required = Count - Spare;
    return S_OK;
}

/**
 * @brief
 * Takes the services this device said it could not do without.
 *
 * @remarks
 * The provider is not asked for them directly. It is asked for the one
 * interface that hands services out, and each service is asked of that. A
 * device that is given a provider which cannot do even that has been put
 * somewhere it does not belong, and says so rather than going on.
 */
STDMETHODIMP VirtualDeviceBase::Initialize(void *Repository, ULONG_PTR Reserved,
                                           IUnknown *Provider)
{
    UNREFERENCED_PARAMETER(Repository);
    UNREFERENCED_PARAMETER(Reserved);

    if (Provider == nullptr)
        return E_POINTER;

    HRESULT Status = Provider->QueryInterface(IID_IVmServiceAccess,
                                              reinterpret_cast<void **>(&m_Access));

    if (FAILED(Status))
        return Status;

    Status = m_Access->GetService(IID_IVmAmd64EmulationServices,
                                  reinterpret_cast<void **>(&m_Emulation));

    if (SUCCEEDED(Status))
    {
        /* Not every device is given one, and none of them fail for want of it */
        m_Access->GetService(IID_IVmProcessorServices,
                             reinterpret_cast<void **>(&m_Processors));
    }

    return Status;
}

HRESULT VirtualDeviceBase::FindService(REFIID Service, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    *Object = nullptr;

    if (m_Access == nullptr)
        return E_UNEXPECTED;

    return m_Access->GetService(Service, Object);
}

STDMETHODIMP VirtualDeviceBase::Teardown()
{
    FreeReservations();

    if (m_Processors != nullptr)
    {
        m_Processors->Release();
        m_Processors = nullptr;
    }

    if (m_Emulation != nullptr)
    {
        m_Emulation->Release();
        m_Emulation = nullptr;
    }

    if (m_Access != nullptr)
    {
        m_Access->Release();
        m_Access = nullptr;
    }

    return S_OK;
}

HRESULT VirtualDeviceBase::ReservePorts(USHORT First, USHORT Last,
                                        IVndIoPortHandler *Handler)
{
    if (m_Emulation == nullptr)
        return E_UNEXPECTED;

    if (m_Reservations >= MaxReservations)
        return E_OUTOFMEMORY;

    IVndRegistration *Registration = nullptr;

    const HRESULT Status = m_Emulation->RegisterIoPortHandler(First, Last,
                                                              VDEV_WIDTH_ANY,
                                                              Handler, 0,
                                                              &Registration);

    if (SUCCEEDED(Status) && (Registration != nullptr))
        m_Reserved[m_Reservations++] = Registration;

    return Status;
}

HRESULT VirtualDeviceBase::ReserveMemory(ULONG64 Base, ULONG64 Length,
                                         IVndMmioHandler *Handler)
{
    if (m_Emulation == nullptr)
        return E_UNEXPECTED;

    if (m_Reservations >= MaxReservations)
        return E_OUTOFMEMORY;

    IVndRegistration *Registration = nullptr;

    const HRESULT Status = m_Emulation->RegisterMmioHandler(
        Base / VDEV_PAGE_SIZE, Length / VDEV_PAGE_SIZE, Handler, TRUE,
        &Registration);

    if (SUCCEEDED(Status) && (Registration != nullptr))
        m_Reserved[m_Reservations++] = Registration;

    return Status;
}

/*
 * Revoked and then let go, in that order. Revoking is what clears the bus;
 * letting go is only this device saying it has finished with the token, and
 * doing it the other way round hands the bus a range nobody answers for.
 */
void VirtualDeviceBase::FreeReservations()
{
    while (m_Reservations > 0)
    {
        IVndRegistration *One = m_Reserved[--m_Reservations];

        m_Reserved[m_Reservations] = nullptr;

        if (One == nullptr)
            continue;

        One->Revoke();
        One->Release();
    }
}

} /* namespace rtvm */
