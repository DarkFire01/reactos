/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The bus everything newer than the board itself is found on
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

namespace rtvm
{

/* As many places as this bus keeps, and how many parts one of them may have */
#define PCI_BUS_DEVICES         16
#define PCI_BUS_FUNCTIONS       8

/* The run of ports the whole thing answers on */
#define PCI_BUS_FIRST_PORT      0x0CF8
#define PCI_BUS_LAST_PORT       0x0CFF

/* Which bits of the address register mean what */
#define PCI_ADDRESS_ENABLED     0x80000000
#define PCI_ADDRESS_KEPT        0x80FFFFFC
#define PCI_ADDRESS_REGISTER    0x000000FC

/* And what a write to the one beside the pair has to have in it to be a restart */
#define PCI_RESET_ASKED         0x06

/* What a place nothing is in answers to everything */
#define PCI_NOTHING_THERE       0xFFFFFFFF

/*
 * The two places that are the board rather than anything loaded into it: the
 * way from the processor onto the bus, and the way from the bus back to the
 * addresses that were there before there was a bus at all.
 */
#define PCI_BRIDGE_DEVICE       0
#define PCI_BRIDGE_FUNCTION     0
#define PCI_LEGACY_DEVICE       7
#define PCI_LEGACY_FUNCTION     0

/* Where in the bytes describing a device each of the fixed ones is */
#define PCI_WHO_IT_IS           0x00
#define PCI_WHAT_IT_IS_DOING    0x04
#define PCI_WHAT_IT_IS          0x08
#define PCI_HOW_IT_IS_LAID_OUT  0x0C

/* The two bits of the command register a device of this machine ever has */
#define PCI_DECODES_PORTS       0x0001
#define PCI_DECODES_MEMORY      0x0002
#define PCI_MOVES_ITS_OWN       0x0004

class PciBusDevice : public VirtualDeviceBase,
                     public IVndIoPortHandler,
                     public IVmPciBusService
{
public:
    PciBusDevice();
    ~PciBusDevice() override;

    PciBusDevice(const PciBusDevice &) = delete;
    PciBusDevice &operator=(const PciBusDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Optional) override;
    STDMETHODIMP StartReservingResources() override;
    STDMETHODIMP Reset() override;

    /* Where a device asks for a place on it */
    STDMETHODIMP InstallPciDevice(IVmPciConfigAccessHandler *Handler,
                                  UCHAR Device, UCHAR Function,
                                  IVmInstalledPciDevice **Installed) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value) override;

private:
    /* Whether an access of that width at that port reaches the data register */
    static bool Addressed(USHORT Port, ULONG Width);

    /* Which place on this bus the address register is pointing at, if any */
    bool Aimed(UCHAR &Device, UCHAR &Function, USHORT &Offset) const;

    ULONG Ask(UCHAR Device, UCHAR Function, USHORT Offset);
    void Tell(UCHAR Device, UCHAR Function, USHORT Offset, ULONG Value);

    /*
     * The places on the bus that are the board. Nothing is loaded for them and
     * nothing may take them: a bus with nothing at the first place is read as
     * a bus that is not there.
     */
    static bool IsBoard(UCHAR Device, UCHAR Function);
    static ULONG Bridge(USHORT Offset);
    static ULONG Legacy(USHORT Offset);

    CRITICAL_SECTION m_Lock = {};

    ULONG m_Address = 0;

    IVmPciConfigAccessHandler *m_Slot[PCI_BUS_DEVICES][PCI_BUS_FUNCTIONS] = {};
};

} /* namespace rtvm */
