/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The pair of interrupt controllers a PC has always had
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Two chips, the second feeding the first through line two. A device holds a
 * line up, the first chip works out whether anything unmasked outranks what is
 * already being serviced, and the processors are told which vector is owed.
 *
 * Nothing here asks whether the guest is willing to take an interrupt. That is
 * not a question a controller has ever been able to answer, and the answer
 * arrives far too late to be worth waiting for.
 */

#include "picdevice.h"

namespace rtvm
{

/* Where the chips answer */
#define PIC_MASTER_COMMAND  0x20
#define PIC_MASTER_DATA     0x21
#define PIC_SLAVE_COMMAND   0xA0
#define PIC_SLAVE_DATA      0xA1

/* The line the second chip arrives on */
#define PIC_CASCADE_LINE    2

/* Command words */
#define ICW1_SINGLE         0x02
#define ICW1_INITIALISE     0x10
#define ICW4_AUTO_END       0x02

#define OCW2_END_OF_INTERRUPT 0x20
#define OCW2_SPECIFIC         0x40
#define OCW3_SELECT           0x08
#define OCW3_READ_SERVICE     0x01

/* WHAT IT IS *****************************************************************/

PicDevice::PicDevice()
{
    InitializeCriticalSection(&m_Lock);
    Clear();
}

PicDevice::~PicDevice()
{
    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP PicDevice::QueryInterface(REFIID Interface, void **Object)
{
    if (Object == nullptr)
        return E_POINTER;

    if (IsEqualIID(Interface, IID_IUnknown) ||
        IsEqualIID(Interface, IID_IVirtualDevice))
    {
        *Object = static_cast<IVirtualDevice *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVndIoPortHandler))
    {
        *Object = static_cast<IVndIoPortHandler *>(this);
    }
    else if (IsEqualIID(Interface, IID_IVmPicService))
    {
        *Object = static_cast<IVmPicService *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

/* BEING BROUGHT UP ***********************************************************/

STDMETHODIMP PicDevice::GetDependencies(void *Repository, ULONG *Count,
                                        GUID **Services, ULONG *Optional)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IVmProcessorServices
    };

    UNREFERENCED_PARAMETER(Repository);

    return PublishDependencies(Wanted, ARRAYSIZE(Wanted),
                               Count, Services, Optional);
}

STDMETHODIMP PicDevice::StartReservingResources()
{
    HRESULT Status = ReservePorts(PIC_MASTER_COMMAND, PIC_MASTER_DATA, this);

    if (SUCCEEDED(Status))
        Status = ReservePorts(PIC_SLAVE_COMMAND, PIC_SLAVE_DATA, this);

    return Status;
}

STDMETHODIMP PicDevice::PowerOnCold()
{
    Reset();
    return S_OK;
}

STDMETHODIMP PicDevice::Reset()
{
    EnterCriticalSection(&m_Lock);
    Clear();
    LeaveCriticalSection(&m_Lock);

    Offer();
    return S_OK;
}

void PicDevice::Clear()
{
    memset(m_Chip, 0, sizeof(m_Chip));

    /* Everything masked until the firmware says which lines it wants */
    m_Chip[0].Mask = 0xFF;
    m_Chip[1].Mask = 0xFF;

    /* Where the firmware puts them, and where it leaves them */
    m_Chip[0].Base = 0x08;
    m_Chip[1].Base = 0x70;

    m_Offered = VDEV_NO_VECTOR;
    m_OfferedLine = -1;
    m_Outstanding = false;
}

/* THE LINES ******************************************************************/

/**
 * @brief
 * Follows a device's line, and remembers that it went up.
 *
 * @remarks
 * The request register latches. A device that pulses its line, which is what
 * the interval timer does, is gone again long before anything looks, so a
 * register that only mirrored the wire would show nothing and the interrupt
 * would be lost.
 *
 * The state of the wire is kept alongside it because the two are not the same
 * question: the latch says an interrupt is owed, and the wire says whether the
 * device is still asking. One that holds its line up wants another interrupt
 * after each is answered, and one that pulsed does not.
 */
void PicDevice::Follow(ULONG Line, bool Asserted)
{
    Chip &Chip = m_Chip[Line >= 8 ? 1 : 0];
    const UCHAR Bit = (UCHAR)(1u << (Line & 7));

    if (Asserted)
    {
        /* Only a rising edge latches, so holding a line up is not a stream */
        if (!(Chip.Level & Bit))
            Chip.Request |= Bit;

        Chip.Level |= Bit;
    }
    else
    {
        Chip.Level &= (UCHAR)~Bit;
    }

    if (Line >= 8)
        Cascade();
}

/**
 * @brief
 * Carries the second chip's answer up to the line it is wired to on the first.
 *
 * @remarks
 * The two are not peers. Only the first is wired to the processors, and the
 * second reaches them by holding the first one's line two. Without this every
 * line from eight upward is latched and then never looked at, which is every
 * interrupt from the clock and the disk.
 */
void PicDevice::Cascade()
{
    const bool Asking = (m_Chip[1].Request & ~m_Chip[1].Mask) != 0;
    const UCHAR Bit = (UCHAR)(1u << PIC_CASCADE_LINE);

    if (Asking)
    {
        if (!(m_Chip[0].Level & Bit))
            m_Chip[0].Request |= Bit;

        m_Chip[0].Level |= Bit;
    }
    else
    {
        m_Chip[0].Level &= (UCHAR)~Bit;
    }
}

int PicDevice::Highest(const Chip &Chip)
{
    const UCHAR Ready = (UCHAR)(Chip.Request & ~Chip.Mask);

    for (int Index = 0; Index < 8; Index++)
    {
        const UCHAR Bit = (UCHAR)(1u << Index);

        /*
         * Anything already being serviced blocks everything at or below its own
         * priority, which is what makes a nested handler work.
         */
        if (Chip.Service & Bit)
            return -1;

        if (Ready & Bit)
            return Index;
    }

    return -1;
}

/**
 * @brief
 * Works out which vector is owed and tells the processors, if it has changed.
 *
 * @remarks
 * Only the change is worth reporting. A controller that said the same thing
 * after every port write would have the processors doing the same work over
 * and again for nothing, and a controller that said nothing would leave an
 * interrupt owed and never taken.
 */
void PicDevice::Offer()
{
    ULONG Vector = VDEV_NO_VECTOR;

    EnterCriticalSection(&m_Lock);

    const int Line = Highest(m_Chip[0]);

    if (Line >= 0)
    {
        if (Line != PIC_CASCADE_LINE)
        {
            Vector = (ULONG)(m_Chip[0].Base + Line);
        }
        else
        {
            const int Slave = Highest(m_Chip[1]);

            /* It may have changed its mind between raising the line and now */
            Vector = (Slave >= 0)
                   ? (ULONG)(m_Chip[1].Base + Slave)
                   : (ULONG)(m_Chip[0].Base + PIC_CASCADE_LINE);
        }
    }

    const bool Changed = (Vector != m_Offered);

    m_Offered = Vector;
    m_OfferedLine = Line;

    /*
     * Told while the lock is still held. Three threads reach this, and one
     * that worked out an answer and then let go before giving it can be
     * overtaken by another that worked out a different one, leaving the
     * processors holding the older of the two and this holding the newer.
     *
     * Nothing the processors do in here comes back this way, so holding it
     * across the call costs nothing.
     */
    if (Changed && (Processors() != nullptr))
    {
        Processors()->SetPendingInterrupt(VDEV_INTERRUPT_FROM_PIC, 0, Vector);
        m_Outstanding = (Vector != VDEV_NO_VECTOR);
    }

    LeaveCriticalSection(&m_Lock);
}

/**
 * @brief
 * Puts whatever was offered into service, if the processor has taken it.
 *
 * @remarks
 * This runs before anything else the guest does, and that order is the whole
 * point of it. A handler's first act is to say the interrupt is finished, and
 * one that arrived before the vector was in service would clear nothing and
 * leave the line held down for good.
 *
 * A real controller has none of this trouble: the cycle that acknowledges the
 * interrupt puts it in service, and that cycle is long over before the guest
 * runs an instruction. There is no such cycle here, so the processor is asked.
 */
void PicDevice::Settle()
{
    EnterCriticalSection(&m_Lock);

    if (m_Outstanding && (Processors() != nullptr) &&
        SUCCEEDED(Processors()->TakePendingInterrupt()))
    {
        Service();
    }

    LeaveCriticalSection(&m_Lock);
}

void PicDevice::Service()
{
    m_Outstanding = false;

    if (m_OfferedLine < 0)
        return;

    Take(m_Chip[0], m_OfferedLine);

    if (m_OfferedLine == PIC_CASCADE_LINE)
    {
        const int Slave = Highest(m_Chip[1]);

        if (Slave >= 0)
            Take(m_Chip[1], Slave);
    }

    m_OfferedLine = -1;
    m_Offered = VDEV_NO_VECTOR;
}

STDMETHODIMP PicDevice::AssertIrq(ULONG Line)
{
    if (Line > 15)
        return E_INVALIDARG;

    Settle();

    EnterCriticalSection(&m_Lock);
    Follow(Line, true);
    LeaveCriticalSection(&m_Lock);

    Offer();
    return S_OK;
}

STDMETHODIMP PicDevice::DeassertIrq(ULONG Line)
{
    if (Line > 15)
        return E_INVALIDARG;

    Settle();

    EnterCriticalSection(&m_Lock);
    Follow(Line, false);
    LeaveCriticalSection(&m_Lock);

    Offer();
    return S_OK;
}

/**
 * @brief
 * Marks the vector that was offered as taken, and works out the next.
 *
 * @remarks
 * Taking one clears what was owed. If the device is still holding its line up
 * it is owed another straight away, which is what keeps a level like the serial
 * port's being served until the device itself lets go.
 */
STDMETHODIMP PicDevice::EndOfInterrupt(ULONG Line)
{
    if (Line > 15)
        return E_INVALIDARG;

    Settle();

    EnterCriticalSection(&m_Lock);

    Chip &Chip = m_Chip[Line >= 8 ? 1 : 0];

    Chip.Service &= (UCHAR)~(1u << (Line & 7));

    LeaveCriticalSection(&m_Lock);

    Offer();
    return S_OK;
}

/**
 * @brief
 * Moves a line from being owed to being served.
 *
 * @remarks
 * What is owed is cleared and is not owed again until the wire goes up afresh,
 * however long it stays up in the meantime. That is what being triggered by an
 * edge means, and a device that holds its line down while it is being dealt
 * with is the ordinary case rather than a reason to interrupt again.
 *
 * Putting it back because the wire was still held is what a chip wired for
 * levels would do, and on a machine of this kind it means a device that takes
 * a moment to finish is asked about a million times on the way.
 */
void PicDevice::Take(Chip &Chip, int Line)
{
    const UCHAR Bit = (UCHAR)(1u << Line);

    Chip.Service |= Bit;
    Chip.Request &= (UCHAR)~Bit;

    if (Chip.AutoEnd)
        Chip.Service &= (UCHAR)~Bit;
}

/* THE REGISTERS **************************************************************/

STDMETHODIMP PicDevice::NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value)
{
    UNREFERENCED_PARAMETER(Width);

    if (Value == nullptr)
        return E_POINTER;

    Settle();

    EnterCriticalSection(&m_Lock);

    Chip &Chip = m_Chip[((Port & 0x80) != 0) ? 1 : 0];

    if ((Port == PIC_MASTER_COMMAND) || (Port == PIC_SLAVE_COMMAND))
        *Value = Chip.ReadService ? Chip.Service : Chip.Request;
    else
        *Value = Chip.Mask;

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP PicDevice::NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value)
{
    const UCHAR Byte = (UCHAR)(Value & 0xFF);
    const bool IsCommand = (Port == PIC_MASTER_COMMAND) ||
                           (Port == PIC_SLAVE_COMMAND);

    UNREFERENCED_PARAMETER(Width);

    Settle();

    EnterCriticalSection(&m_Lock);

    Chip &Chip = m_Chip[((Port & 0x80) != 0) ? 1 : 0];

    if (IsCommand)
    {
        if (Byte & ICW1_INITIALISE)
        {
            /* The whole chip goes back to the start, mask and all */
            Chip.InitStep = 1;
            Chip.Mask = 0;
            Chip.Service = 0;
            Chip.Cascade = (Byte & ICW1_SINGLE) == 0;
            Chip.AutoEnd = false;
        }
        else if (Byte & OCW3_SELECT)
        {
            /* Picking which register the command port reads back */
            Chip.ReadService = (Byte & OCW3_READ_SERVICE) != 0;
        }
        else if (Byte & OCW2_END_OF_INTERRUPT)
        {
            if (Byte & OCW2_SPECIFIC)
            {
                Chip.Service &= (UCHAR)~(1u << (Byte & 7));
            }
            else
            {
                /* The one being serviced at the highest priority goes */
                for (int Index = 0; Index < 8; Index++)
                {
                    const UCHAR Bit = (UCHAR)(1u << Index);

                    if (Chip.Service & Bit)
                    {
                        Chip.Service &= (UCHAR)~Bit;
                        break;
                    }
                }
            }
        }
    }
    else
    {
        switch (Chip.InitStep)
        {
            case 1:
                /* Where this chip's eight lines land in the table */
                Chip.Base = (UCHAR)(Byte & 0xF8);
                Chip.InitStep = Chip.Cascade ? 2 : 3;
                break;

            case 2:
                /* Which line the other chip is on, which is already known */
                Chip.InitStep = 3;
                break;

            case 3:
                Chip.AutoEnd = (Byte & ICW4_AUTO_END) != 0;
                Chip.InitStep = 0;
                break;

            default:
                Chip.Mask = Byte;

                /* Unmasking on the second chip may be it starting to ask */
                Cascade();
                break;
        }
    }

    LeaveCriticalSection(&m_Lock);

    Offer();
    return S_OK;
}

} /* namespace rtvm */
