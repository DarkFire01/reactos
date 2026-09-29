/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The pair of interrupt controllers a PC has always had
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

class PicDevice : public VirtualDeviceBase,
                  public IVndIoPortHandler,
                  public IVmPicService
{
public:
    PicDevice();
    ~PicDevice() override;

    PicDevice(const PicDevice &) = delete;
    PicDevice &operator=(const PicDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Optional) override;
    STDMETHODIMP StartReservingResources() override;
    STDMETHODIMP PowerOnCold() override;
    STDMETHODIMP Reset() override;

    /* What a device raising a line sees */
    STDMETHODIMP EndOfInterrupt(ULONG Line) override;
    STDMETHODIMP AssertIrq(ULONG Line) override;
    STDMETHODIMP DeassertIrq(ULONG Line) override;

    /* The four addresses the pair answers at */
    STDMETHODIMP Unknown3() override { return E_NOTIMPL; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value) override;

private:
    struct Chip
    {
        /* What is owed, latched, and what the wire is doing right now */
        UCHAR Request;
        UCHAR Level;
        UCHAR Service;
        UCHAR Mask;
        UCHAR Base;
        UCHAR InitStep;
        bool Cascade;
        bool AutoEnd;
        bool ReadService;
    };

    void Clear();
    void Follow(ULONG Line, bool Asserted);
    void Cascade();
    void Offer();
    void Settle();
    void Service();
    static int Highest(const Chip &Chip);
    static void Take(Chip &Chip, int Line);

    CRITICAL_SECTION m_Lock = {};
    Chip m_Chip[2] = {};

    /* What the processors were last told, so that only a change is told again */
    ULONG m_Offered = VDEV_NO_VECTOR;
    int m_OfferedLine = -1;

    /* Whether that offer has still to be answered */
    volatile bool m_Outstanding = false;
};

} /* namespace rtvm */
