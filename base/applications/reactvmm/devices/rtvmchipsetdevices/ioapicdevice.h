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

/* How many of them the old pair of chips is wired to as well */
#define IOAPIC_LEGACY_LINES 16

/* The window it answers in, and the two registers inside it */
#define IOAPIC_WINDOW_SIZE  0x1000
#define IOAPIC_SELECT       0x00
#define IOAPIC_VALUE        0x10

/* The three that describe the part, and where the table starts after them */
#define IOAPIC_WHICH_ONE    0x00
#define IOAPIC_VERSION      0x01
#define IOAPIC_SHARING      0x02
#define IOAPIC_FIRST_LINE   0x10

/* What it says it is, and how many lines it says it has */
#define IOAPIC_VERSION_SAID 0x11

/* The bits of a line's own entry that say what to do with it */
#define IOAPIC_VECTOR_MASK  0x000000FF
#define IOAPIC_DELIVERY_AT  8
#define IOAPIC_DELIVERY_OF  7
#define IOAPIC_IS_LOGICAL   0x00000800
#define IOAPIC_IS_LEVEL     0x00008000
#define IOAPIC_IS_MASKED    0x00010000
#define IOAPIC_WHERE_AT     56

class IoApicDevice : public VirtualDeviceBase,
                     public IVmIoApic,
                     public IVndMmioHandler
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
                                 GUID **Services, ULONG *Required) override;
    STDMETHODIMP PowerOnCold(VDEV_STATE State) override;
    STDMETHODIMP Reset(VDEV_STATE State) override;

    /* What every device that has a line calls */
    STDMETHODIMP WaitForIrqAssert(ULONG Line) override;
    STDMETHODIMP AssertIrq(UCHAR Line, UCHAR Source) override;
    STDMETHODIMP DeassertIrq(UCHAR Line, UCHAR Source) override;
    STDMETHODIMP RequestTimerAssist(UCHAR Line, ULONG64 Period,
                                    int *Assisted, int *StillWanted) override;
    STDMETHODIMP DeclineTimerAssist(UCHAR Line) override;
    STDMETHODIMP RegisterRteChangeCallback(UCHAR Line,
                                           IUnknown *Callback) override;
    STDMETHODIMP UnregisterRteChangeCallback(UCHAR Line) override;
    STDMETHODIMP SetIoApicBaseAddress(ULONG Address) override;

    STDMETHODIMP StartReservingResources(void *Repository, VDEV_STATE State) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyMmioRead(ULONG64 Address, ULONG Length,
                                void *Buffer) override;
    STDMETHODIMP NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                 const void *Buffer) override;

private:
    struct Entry
    {
        /* Where the line goes, as the guest wrote it */
        ULONG64 Redirection;

        /* Who wants to know when that changes */
        IUnknown *Watcher;

        /* Which of the devices on it are holding it, one bit each */
        ULONG Held;
    };

    ULONG Register(ULONG Which) const;
    void Write(ULONG Which, ULONG Value);
    void Deliver(ULONG Line);

    CRITICAL_SECTION m_Lock = {};
    Entry m_Line[IOAPIC_LINE_COUNT] = {};
    ULONG64 m_Base = VDEV_IOAPIC_DEFAULT_BASE;

    /* Which of the registers inside the window the guest last named */
    UCHAR m_Selected = 0;
    UCHAR m_WhichOne = 0;

    /* The pair of chips, wired to the same lines and masking for itself */
    IVmPicService *m_Legacy = nullptr;

    /* And what puts one in front of a processor once this decides where */
    IVmProcessorServices *m_Processors = nullptr;
};

} /* namespace rtvm */
