/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What every device in this library has in common
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <windows.h>
#include <objbase.h>

#include "vdev.h"

namespace rtvm
{

/* How many devices this library has handed out and not been given back */
extern volatile LONG g_Outstanding;

/**
 * @brief
 * The slots every device has and almost none of them do anything with.
 *
 * @remarks
 * A device is driven through a long run of calls, and most devices care about
 * four or five of them. Answering the rest here rather than in each device is
 * what keeps a device to the part of it that is actually a piece of hardware.
 */
class VirtualDeviceBase : public IVirtualDevice
{
public:
    VirtualDeviceBase() noexcept { InterlockedIncrement(&g_Outstanding); }
    virtual ~VirtualDeviceBase() { InterlockedDecrement(&g_Outstanding); }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG Left = InterlockedDecrement(&m_Count);

        if (Left == 0)
            delete this;

        return (ULONG)Left;
    }

    /*
     * A device that wants nothing says so by answering with an empty list. The
     * ones that want something override this.
     */
    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Required) override
    {
        UNREFERENCED_PARAMETER(Repository);

        if (Services != nullptr)
            *Services = nullptr;

        if (Count != nullptr)
            *Count = 0;

        if (Required != nullptr)
            *Required = 0;

        return S_OK;
    }

    STDMETHODIMP Initialize(void *Repository, ULONG_PTR Reserved,
                            IUnknown *Provider) override;
    STDMETHODIMP Teardown() override;

    STDMETHODIMP StartReservingResources(void *Repository,
                                         VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(Repository);
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP FinishReservingResources(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP FreeReservedResources() override
    {
        FreeReservations();
        return S_OK;
    }

    STDMETHODIMP SaveReservedResources(void *Repository) override
    {
        UNREFERENCED_PARAMETER(Repository);
        return S_OK;
    }

    STDMETHODIMP PowerOnCold(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    /*
     * Nothing here keeps anything worth writing out, so coming back into a
     * state that was saved is the same as coming up from nothing.
     */
    STDMETHODIMP PowerOnRestore(void *Repository, VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(Repository);
        return PowerOnCold(State);
    }

    STDMETHODIMP PowerOff(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP Save(void *Repository, VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(Repository);
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP Resume(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP Pause(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    /* Nothing here is fast enough to be worth an optimization to turn off */
    STDMETHODIMP EnableOptimizations(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP StartDisableOptimizations(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP FinishDisableOptimizations(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP Reset(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

    STDMETHODIMP PostReset(VDEV_STATE State) override
    {
        UNREFERENCED_PARAMETER(State);
        return S_OK;
    }

protected:
    /* Whatever the manager gave, from the moment Initialize was called */
    IVmAmd64EmulationServices *Emulation() const noexcept { return m_Emulation; }
    IVmProcessorServices *Processors() const noexcept { return m_Processors; }

    /* Handed the ports this device answers for, and the handler to reach it by */
    HRESULT ReservePorts(USHORT First, USHORT Last, IVndIoPortHandler *Handler);

    /* And the same for a window of memory, which is counted in pages */
    HRESULT ReserveMemory(ULONG64 Base, ULONG64 Length,
                          IVndMmioHandler *Handler);

    /*
     * Anything else the manager has, asked for after the fact. A device that
     * leans on another device asks here rather than when it was initialised,
     * because one device coming up is no promise that another already has.
     */
    HRESULT FindService(REFIID Service, void **Object);

    /* Gives every range this device reserved back, in the order it took them */
    void FreeReservations();

private:
    /* As many runs of ports as any one device here asks for */
    static const ULONG MaxReservations = 8;

    volatile LONG m_Count = 1;
    IVmServiceAccess *m_Access = nullptr;
    IVmAmd64EmulationServices *m_Emulation = nullptr;
    IVmProcessorServices *m_Processors = nullptr;

    IVndRegistration *m_Reserved[MaxReservations] = {};
    ULONG m_Reservations = 0;
};

/* Fills in a list of identifiers for GetDependencies, allocated as it must be */
HRESULT PublishDependencies(_In_reads_(Count) const GUID *const *Wanted,
                            _In_ ULONG Count,
                            _In_ ULONG Spare,
                            _Out_ ULONG *Answered,
                            _Outptr_ GUID **Services,
                            _Out_ ULONG *Required);

} /* namespace rtvm */
