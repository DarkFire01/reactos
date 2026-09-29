/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Where a line goes once a device has raised it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A device does not raise its line on the interrupt controller. It raises it
 * here, and where it goes from here is whatever the guest has set up: a message
 * to one processor, a message to several, or the pair of chips a machine of
 * this kind has always had.
 *
 * Until a guest sets anything up it is the pair of chips, which is why they are
 * asked for and why not having them is not a reason to refuse to come up: a
 * machine put together without them is one whose guest was always going to do
 * the setting up itself.
 */

#include "ioapicdevice.h"

namespace rtvm
{

IoApicDevice::IoApicDevice()
{
    InitializeCriticalSection(&m_Lock);
}

IoApicDevice::~IoApicDevice()
{
    for (Entry &Line : m_Line)
    {
        if (Line.Watcher != nullptr)
            Line.Watcher->Release();
    }

    if (m_Legacy != nullptr)
        m_Legacy->Release();

    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP IoApicDevice::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVirtualDevice))
    {
        *Object = static_cast<IVirtualDevice *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVmIoApic))
    {
        *Object = static_cast<IVmIoApic *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP IoApicDevice::GetDependencies(void *Repository, ULONG *Count,
                                           GUID **Services, ULONG *Optional)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IVmProcessorServices,
        &IID_IVmPicService
    };

    UNREFERENCED_PARAMETER(Repository);

    const HRESULT Status = PublishDependencies(Wanted, ARRAYSIZE(Wanted),
                                               Count, Services, Optional);

    /* The last of them is one this will come up without */
    if (SUCCEEDED(Status) && (Optional != nullptr))
        *Optional = 1u << 2;

    return Status;
}

STDMETHODIMP IoApicDevice::PowerOnCold()
{
    /*
     * Asked for here rather than when the services arrived, because the chips
     * are a device too and one device is not entitled to assume another came
     * up before it did.
     */
    if (m_Legacy == nullptr)
        FindService(IID_IVmPicService, reinterpret_cast<void **>(&m_Legacy));

    return Reset();
}

STDMETHODIMP IoApicDevice::Reset()
{
    EnterCriticalSection(&m_Lock);

    for (Entry &Line : m_Line)
    {
        /*
         * Every line masked, which is the one bit of a reset table that means
         * anything: nothing is delivered through here until the guest has said
         * where it should go.
         */
        Line.Redirection = 0x10000;
    }

    m_Base = VDEV_IOAPIC_DEFAULT_BASE;

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/* THE LINES ******************************************************************/

/**
 * @brief
 * Carries a line to wherever the guest has said it goes.
 *
 * @remarks
 * There is nowhere else for one of the first sixteen to go yet. A guest that
 * has written a redirection table wants messages sent to processors, and the
 * table is kept and reported but not yet acted on; everything this machine runs
 * leaves it alone and expects the chips.
 */
STDMETHODIMP IoApicDevice::AssertIrq(ULONG Line)
{
    if (Line >= IOAPIC_LINE_COUNT)
        return E_INVALIDARG;

    if ((Line < 16) && (m_Legacy != nullptr))
        return m_Legacy->AssertIrq(Line);

    return S_FALSE;
}

STDMETHODIMP IoApicDevice::DeassertIrq(ULONG Line)
{
    if (Line >= IOAPIC_LINE_COUNT)
        return E_INVALIDARG;

    if ((Line < 16) && (m_Legacy != nullptr))
        return m_Legacy->DeassertIrq(Line);

    return S_FALSE;
}

/*
 * For a device that would rather block than be called back. Nothing here has
 * asked, and answering that it cannot is better than a wait that never ends.
 */
STDMETHODIMP IoApicDevice::WaitForIrqAssert(ULONG Line)
{
    UNREFERENCED_PARAMETER(Line);

    return E_NOTIMPL;
}

/*
 * A device whose line is a clock asking to be left alone between ticks. Taken
 * and nothing done, because a machine this size gains nothing by it and a
 * device that asked will tick the ordinary way instead.
 */
STDMETHODIMP IoApicDevice::RequestTimerAssist(ULONG Line)
{
    UNREFERENCED_PARAMETER(Line);

    return S_FALSE;
}

STDMETHODIMP IoApicDevice::DeclineTimerAssist(ULONG Line)
{
    UNREFERENCED_PARAMETER(Line);

    return S_OK;
}

STDMETHODIMP IoApicDevice::RegisterRteChangeCallback(ULONG Line,
                                                     IUnknown *Callback)
{
    if ((Line >= IOAPIC_LINE_COUNT) || (Callback == nullptr))
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);

    if (m_Line[Line].Watcher != nullptr)
        m_Line[Line].Watcher->Release();

    m_Line[Line].Watcher = Callback;
    Callback->AddRef();

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP IoApicDevice::UnregisterRteChangeCallback(ULONG Line,
                                                       IUnknown *Callback)
{
    if (Line >= IOAPIC_LINE_COUNT)
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);

    if (m_Line[Line].Watcher == Callback)
    {
        m_Line[Line].Watcher->Release();
        m_Line[Line].Watcher = nullptr;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP IoApicDevice::SetIoApicBaseAddress(ULONG64 Address)
{
    EnterCriticalSection(&m_Lock);
    m_Base = Address;
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

} /* namespace rtvm */
