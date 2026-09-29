/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Where a line goes once a device has raised it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

/* How many lines the table has room for */
#define IOAPIC_LINE_COUNT 24

class IoApicDevice : public VirtualDeviceBase, public IVmIoApic
{
public:
    IoApicDevice();
    ~IoApicDevice() override;

    IoApicDevice(const IoApicDevice &) = delete;
    IoApicDevice &operator=(const IoApicDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Optional) override;
    STDMETHODIMP PowerOnCold() override;
    STDMETHODIMP Reset() override;

    /* What every device that has a line calls */
    STDMETHODIMP WaitForIrqAssert(ULONG Line) override;
    STDMETHODIMP AssertIrq(ULONG Line) override;
    STDMETHODIMP DeassertIrq(ULONG Line) override;
    STDMETHODIMP RequestTimerAssist(ULONG Line) override;
    STDMETHODIMP DeclineTimerAssist(ULONG Line) override;
    STDMETHODIMP RegisterRteChangeCallback(ULONG Line,
                                           IUnknown *Callback) override;
    STDMETHODIMP UnregisterRteChangeCallback(ULONG Line,
                                             IUnknown *Callback) override;
    STDMETHODIMP SetIoApicBaseAddress(ULONG64 Address) override;

private:
    struct Entry
    {
        /* Where the line goes, as the guest wrote it */
        ULONG64 Redirection;

        /* Who wants to know when that changes */
        IUnknown *Watcher;
    };

    CRITICAL_SECTION m_Lock = {};
    Entry m_Line[IOAPIC_LINE_COUNT] = {};
    ULONG64 m_Base = VDEV_IOAPIC_DEFAULT_BASE;

    /* The pair of chips, which is where a line goes until the guest says else */
    IVmPicService *m_Legacy = nullptr;
};

} /* namespace rtvm */
