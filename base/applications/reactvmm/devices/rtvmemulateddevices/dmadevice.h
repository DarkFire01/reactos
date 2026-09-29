/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The pair of transfer controllers a PC has always had
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

/* Which way a channel was programmed to move data, for RequestDma */
#define DMA_TO_MEMORY       1
#define DMA_FROM_MEMORY     2
#define DMA_NEITHER_WAY     0

/* How a request went */
#define DMA_REQUEST_TAKEN   0
#define DMA_REQUEST_REFUSED 1

class DmaDevice : public VirtualDeviceBase,
                  public IVndIoPortHandler,
                  public IVmDmaController
{
public:
    DmaDevice();
    ~DmaDevice() override;

    DmaDevice(const DmaDevice &) = delete;
    DmaDevice &operator=(const DmaDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Optional) override;
    STDMETHODIMP StartReservingResources() override;
    STDMETHODIMP PowerOnCold() override;
    STDMETHODIMP Reset() override;

    /* What a device that moves its own data sees */
    STDMETHODIMP GetDmaChannelCount(ULONG *Count) override;
    STDMETHODIMP RequestDma(ULONG Channel, double Unknown, ULONG Length,
                            ULONG *Direction, ULONG64 *Address, ULONG *Count,
                            ULONG *Result) override;
    STDMETHODIMP ReportDmaComplete(ULONG Channel) override;

    STDMETHODIMP Unknown3() override { return E_NOTIMPL; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value) override;

private:
    struct ChannelState
    {
        /* What is programmed, and how far through it the transfer is */
        USHORT BaseAddress;
        USHORT BaseCount;
        USHORT Address;
        USHORT Count;
        UCHAR Page;
        UCHAR Mode;
        bool Masked;
    };

    struct Chip
    {
        UCHAR Command;
        /* Which channels have run out, and which are asking to move */
        UCHAR Reached;
        UCHAR Asking;

        /* Whether the next half of an address or count is the high one */
        bool HighByte;
    };

    void Clear();
    bool Decode(USHORT Port, ULONG &Which, ULONG &Register) const;
    void Advance(ULONG Channel, ULONG Moved);

    CRITICAL_SECTION m_Lock = {};
    ChannelState m_Channel[8] = {};
    Chip m_Chip[2] = {};

    /* How much the last request on each channel was allowed to move */
    ULONG m_Granted[8] = {};
};

} /* namespace rtvm */
