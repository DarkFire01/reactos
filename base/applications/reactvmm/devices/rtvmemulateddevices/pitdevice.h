/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The three counters a PC keeps time and makes noise with
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

class PitDevice : public VirtualDeviceBase,
                  public IVndIoPortHandler,
                  public IVmPitService
{
public:
    PitDevice();
    ~PitDevice() override;

    PitDevice(const PitDevice &) = delete;
    PitDevice &operator=(const PitDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Optional) override;
    STDMETHODIMP StartReservingResources() override;
    STDMETHODIMP PowerOnCold() override;
    STDMETHODIMP PowerOff() override;
    STDMETHODIMP Reset() override;

    /* What the speaker, and anything else watching a counter, sees */
    STDMETHODIMP EnableSpeakerTimer(BOOL Enabled) override;
    STDMETHODIMP GetTimerOutputSignal(ULONG Counter, BOOL *High) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value) override;

private:
    struct Counter
    {
        USHORT Reload;
        USHORT Latched;
        UCHAR Access;
        UCHAR Mode;
        bool Latch;
        bool ReadHigh;
        bool WriteHigh;
        bool Running;
    };

    void Clear();
    void RateChanged();
    static DWORD WINAPI Ticking(LPVOID Parameter);
    void Tick();

    CRITICAL_SECTION m_Lock = {};
    Counter m_Counter[3] = {};

    /* How often the first of them was set up to fire */
    volatile LONG m_Hertz = 18;

    HANDLE m_Thread = nullptr;
    volatile LONG m_Stopping = 0;

    /* Whether the third one is wired to anything that makes a sound */
    bool m_Speaker = false;

    /* Where the line goes, which is not this device's to know */
    IVmIoApic *m_Lines = nullptr;
};

} /* namespace rtvm */
