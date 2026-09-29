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
                                 GUID **Services, ULONG *Optional) override
    {
        UNREFERENCED_PARAMETER(Repository);

        if (Services != nullptr)
            *Services = nullptr;

        if (Count != nullptr)
            *Count = 0;

        if (Optional != nullptr)
            *Optional = 0;

        return S_OK;
    }

    STDMETHODIMP Initialize(void *Repository, ULONG_PTR Reserved,
                            IUnknown *Provider) override;
    STDMETHODIMP Teardown() override;

    STDMETHODIMP StartReservingResources() override { return S_OK; }
    STDMETHODIMP FinishReservingResources() override { return S_OK; }
    STDMETHODIMP FreeReservedResources() override { return S_OK; }
    STDMETHODIMP SaveReservedResources() override { return S_OK; }

    STDMETHODIMP PowerOnCold() override { return S_OK; }
    STDMETHODIMP PowerOnRestore() override { return S_OK; }
    STDMETHODIMP PowerOff() override { return S_OK; }
    STDMETHODIMP Save() override { return S_OK; }
    STDMETHODIMP Resume() override { return S_OK; }
    STDMETHODIMP Pause() override { return S_OK; }

    /* Nothing here is fast enough to be worth an optimisation to turn off */
    STDMETHODIMP EnableOptimizations() override { return S_OK; }
    STDMETHODIMP StartDisableOptimizations() override { return S_OK; }
    STDMETHODIMP FinishDisableOptimizations() override { return S_OK; }

    STDMETHODIMP Reset() override { return S_OK; }
    STDMETHODIMP PostReset() override { return S_OK; }

protected:
    /* Whatever the manager gave, from the moment Initialize was called */
    IVmAmd64EmulationServices *Emulation() const noexcept { return m_Emulation; }
    IVmProcessorServices *Processors() const noexcept { return m_Processors; }

    /* Handed the ports this device answers for, and the handler to reach it by */
    HRESULT ReservePorts(USHORT First, USHORT Last, IVndIoPortHandler *Handler);

    /*
     * Anything else the manager has, asked for after the fact. A device that
     * leans on another device asks here rather than when it was initialised,
     * because one device coming up is no promise that another already has.
     */
    HRESULT FindService(REFIID Service, void **Object);

private:
    volatile LONG m_Count = 1;
    IVmServiceAccess *m_Access = nullptr;
    IVmAmd64EmulationServices *m_Emulation = nullptr;
    IVmProcessorServices *m_Processors = nullptr;
};

/* Fills in a list of identifiers for GetDependencies, allocated as it must be */
HRESULT PublishDependencies(_In_reads_(Count) const GUID *const *Wanted,
                            _In_ ULONG Count,
                            _Out_ ULONG *Answered,
                            _Outptr_ GUID **Services,
                            _Out_ ULONG *Optional);

} /* namespace rtvm */
