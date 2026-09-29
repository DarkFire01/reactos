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

    if (m_Processors != nullptr)
        m_Processors->Release();

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
    else if (IsEqualIID(Interface, IID_IVndMmioHandler))
    {
        *Object = static_cast<IVndMmioHandler *>(this);
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

    if (m_Processors == nullptr)
    {
        FindService(IID_IVmProcessorServices,
                    reinterpret_cast<void **>(&m_Processors));
    }

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
    m_Selected = 0;
    m_WhichOne = 0;

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/* THE LINES ******************************************************************/

/**
 * @brief
 * Puts what a line stands for in front of a processor.
 *
 * @remarks
 * The entry says which vector, where it goes and how, and a masked one says
 * nothing goes anywhere. Only the part that decides is here: what actually
 * reaches a processor is somebody else's, because this has no more idea how a
 * processor is reached than the wire it is standing in for did.
 */
void IoApicDevice::Deliver(ULONG Line)
{
    const ULONG64 Entry = m_Line[Line].Redirection;

    if ((Entry & IOAPIC_IS_MASKED) != 0)
        return;

    if (m_Processors == nullptr)
        return;

    const ULONG Vector = (ULONG)(Entry & IOAPIC_VECTOR_MASK);
    const ULONG64 How = (Entry >> IOAPIC_DELIVERY_AT) & IOAPIC_DELIVERY_OF;

    /*
     * Who it goes to, and the two things that say how that is read: whether
     * the number names a processor or a set of them, and whether the line is
     * held up or only flicked. All three travel together in the one argument
     * that is spare, because the call itself has no room for them.
     */
    ULONG64 Where = (Entry >> IOAPIC_WHERE_AT) & 0xFF;

    if ((Entry & IOAPIC_IS_LOGICAL) != 0)
        Where |= IOAPIC_SAID_LOGICAL;

    if ((Entry & IOAPIC_IS_LEVEL) != 0)
        Where |= IOAPIC_SAID_LEVEL;

    m_Processors->AssertVirtualProcessorInterrupt(How, Where, Vector);
}

/**
 * @brief
 * Carries a line to wherever the guest has said it goes.
 *
 * @remarks
 * Until the guest writes the table, there is nowhere for a line to go but the
 * pair of chips, which is where every machine of this kind starts. Once it has
 * written one, the table is what decides, and a line the table still has
 * masked goes to the chips as it did before: that is how a guest part way
 * through setting this up keeps a clock.
 */
STDMETHODIMP IoApicDevice::AssertIrq(ULONG Line)
{
    if (Line >= IOAPIC_LINE_COUNT)
        return E_INVALIDARG;

    /*
     * The old pair of chips is wired to the same lines as this is, and holds
     * a mask of its own. It is told every time and decides for itself, which
     * is the only arrangement that lets a guest move from one to the other:
     * choosing between them here would drop whatever the guest had not
     * finished setting up.
     */
    if ((Line < IOAPIC_LEGACY_LINES) && (m_Legacy != nullptr))
        m_Legacy->AssertIrq(Line);

    EnterCriticalSection(&m_Lock);

    m_Line[Line].Held = true;

    /* And this one carries it as well, unless its own entry says not to */
    if ((m_Line[Line].Redirection & IOAPIC_IS_MASKED) == 0)
        Deliver(Line);

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP IoApicDevice::DeassertIrq(ULONG Line)
{
    if (Line >= IOAPIC_LINE_COUNT)
        return E_INVALIDARG;

    if ((Line < IOAPIC_LEGACY_LINES) && (m_Legacy != nullptr))
        m_Legacy->DeassertIrq(Line);

    EnterCriticalSection(&m_Lock);
    m_Line[Line].Held = false;
    LeaveCriticalSection(&m_Lock);

    return S_OK;
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

/* THE REGISTERS **************************************************************/

STDMETHODIMP IoApicDevice::StartReservingResources()
{
    if (Emulation() == nullptr)
        return E_UNEXPECTED;

    void *Registration = nullptr;

    return Emulation()->RegisterMmioHandler(m_Base / VDEV_PAGE_SIZE,
                                            IOAPIC_WINDOW_SIZE / VDEV_PAGE_SIZE,
                                            this, TRUE, &Registration);
}

/*
 * Only two registers are in the window. Everything the part holds is reached
 * by naming one in the first and then reading or writing the second, which is
 * how a part with more registers than a window fits in one.
 */
ULONG IoApicDevice::Register(ULONG Which) const
{
    if (Which == IOAPIC_WHICH_ONE)
        return (ULONG)m_WhichOne << 24;

    /* What it is, and one less than how many lines it has */
    if (Which == IOAPIC_VERSION)
        return IOAPIC_VERSION_SAID | ((IOAPIC_LINE_COUNT - 1) << 16);

    if (Which == IOAPIC_SHARING)
        return (ULONG)m_WhichOne << 24;

    if ((Which < IOAPIC_FIRST_LINE) ||
        (Which >= (IOAPIC_FIRST_LINE + (IOAPIC_LINE_COUNT * 2))))
    {
        return 0;
    }

    const ULONG Line = (Which - IOAPIC_FIRST_LINE) / 2;
    const ULONG64 Entry = m_Line[Line].Redirection;

    return ((Which & 1) != 0) ? (ULONG)(Entry >> 32) : (ULONG)Entry;
}

void IoApicDevice::Write(ULONG Which, ULONG Value)
{
    if (Which == IOAPIC_WHICH_ONE)
    {
        m_WhichOne = (UCHAR)((Value >> 24) & 0x0F);
        return;
    }

    /* What it is and who else is on the wire are not the guest's to change */
    if ((Which == IOAPIC_VERSION) || (Which == IOAPIC_SHARING))
        return;

    if ((Which < IOAPIC_FIRST_LINE) ||
        (Which >= (IOAPIC_FIRST_LINE + (IOAPIC_LINE_COUNT * 2))))
    {
        return;
    }

    const ULONG Line = (Which - IOAPIC_FIRST_LINE) / 2;
    ULONG64 Entry = m_Line[Line].Redirection;

    if ((Which & 1) != 0)
        Entry = (Entry & 0x00000000FFFFFFFFull) | ((ULONG64)Value << 32);
    else
        Entry = (Entry & 0xFFFFFFFF00000000ull) | Value;

    m_Line[Line].Redirection = Entry;

    /*
     * A line that is still being held when the guest says where it goes has
     * to go there now. Nothing is going to raise it again: it was raised once
     * and has been waiting ever since for somewhere to be sent.
     */
    if (m_Line[Line].Held && ((Entry & IOAPIC_IS_MASKED) == 0))
        Deliver(Line);
}

STDMETHODIMP IoApicDevice::NotifyMmioRead(ULONG64 Address, ULONG Length,
                                          void *Buffer)
{
    if (Buffer == nullptr)
        return E_INVALIDARG;

    const ULONG64 Offset = Address - m_Base;
    ULONG Value = 0;

    EnterCriticalSection(&m_Lock);

    if ((Offset & ~3ull) == IOAPIC_SELECT)
        Value = m_Selected;
    else if ((Offset & ~3ull) == IOAPIC_VALUE)
        Value = Register(m_Selected);

    LeaveCriticalSection(&m_Lock);

    /* However much of it was asked for, from wherever in it they asked */
    const ULONG Within = (ULONG)(Offset & 3);
    auto *Bytes = static_cast<UCHAR *>(Buffer);

    for (ULONG Index = 0; Index < Length; Index++)
    {
        const ULONG At = Within + Index;

        Bytes[Index] = (At < 4) ? (UCHAR)((Value >> (At * 8)) & 0xFF) : 0;
    }

    return S_OK;
}

STDMETHODIMP IoApicDevice::NotifyMmioWrite(ULONG64 Address, ULONG Length,
                                           const void *Buffer)
{
    if (Buffer == nullptr)
        return E_INVALIDARG;

    const ULONG64 Offset = Address - m_Base;
    const ULONG Within = (ULONG)(Offset & 3);
    const auto *Bytes = static_cast<const UCHAR *>(Buffer);

    EnterCriticalSection(&m_Lock);

    ULONG Value = ((Offset & ~3ull) == IOAPIC_VALUE)
                ? Register(m_Selected)
                : m_Selected;

    for (ULONG Index = 0; Index < Length; Index++)
    {
        const ULONG At = Within + Index;

        if (At >= 4)
            break;

        Value &= ~(0xFFu << (At * 8));
        Value |= (ULONG)Bytes[Index] << (At * 8);
    }

    if ((Offset & ~3ull) == IOAPIC_SELECT)
        m_Selected = (UCHAR)(Value & 0xFF);
    else if ((Offset & ~3ull) == IOAPIC_VALUE)
        Write(m_Selected, Value);

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

} /* namespace rtvm */
