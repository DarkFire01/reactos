/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The clock that keeps running with the machine off
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

/* How many locations are behind the clock */
#define RTC_LOCATIONS 128

class RtcDevice : public VirtualDeviceBase,
                  public IVndIoPortHandler
{
public:
    RtcDevice();
    ~RtcDevice() override;

    RtcDevice(const RtcDevice &) = delete;
    RtcDevice &operator=(const RtcDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Required) override;
    STDMETHODIMP StartReservingResources(void *Repository, VDEV_STATE State) override;
    STDMETHODIMP PowerOnCold(VDEV_STATE State) override;
    STDMETHODIMP PowerOff(VDEV_STATE State) override;
    STDMETHODIMP Reset(VDEV_STATE State) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, USHORT Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, USHORT Width, ULONG Value) override;

private:
    void Load();
    void Stamp();
    UCHAR Encode(_In_ ULONG Value) const;
    void Lower();

    static DWORD WINAPI Ticking(_In_ LPVOID Parameter);
    void Tick();

    CRITICAL_SECTION m_Lock = {};

    UCHAR m_Address = 0;
    UCHAR m_Cmos[RTC_LOCATIONS] = {};

    HANDLE m_Thread = nullptr;
    volatile LONG m_Stopping = 0;

    /* Whether the line is up and waiting for the guest to look */
    volatile LONG m_Raised = 0;

    IVmIoApic *m_Lines = nullptr;
};

} /* namespace rtvm */
