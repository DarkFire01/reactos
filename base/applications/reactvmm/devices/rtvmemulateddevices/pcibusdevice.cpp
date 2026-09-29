/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The bus everything newer than the board itself is found on
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Everything on a board of this age that is not at an address the system
 * already knows is found by asking. Two ports do the asking: one is written
 * with which place on the bus is meant and which four bytes of its description
 * are wanted, and the other carries those four bytes either way.
 *
 * The bus holds none of those bytes itself. It works out which device is being
 * asked about and passes the question on, so a device says what it is without
 * ever learning where on the bus it was put.
 *
 * The first place is the bus's own. A system that finds nothing there decides
 * there is no bus at all and stops looking, so that one answer is the only one
 * this file gives of its own.
 */

#include "pcibusdevice.h"

namespace rtvm
{

/* What the first place says it is, which is a way through and nothing else */
#define BRIDGE_WHO_IT_IS        0x71928086
#define BRIDGE_WHAT_IT_IS_DOING 0x02000006
#define BRIDGE_WHAT_IT_IS       0x06000003

/* And what the way back to the older addresses says it is */
#define LEGACY_WHO_IT_IS        0x71108086
#define LEGACY_WHAT_IT_IS_DOING 0x02800007
#define LEGACY_WHAT_IT_IS       0x06010000

/*
 * That this place has more to it than the one part. Without it nothing looks
 * past the first part, and the controller the disks hang off is the second.
 */
#define LEGACY_HAS_MORE         0x00800000

/* WHAT IT IS *****************************************************************/

PciBusDevice::PciBusDevice()
{
    InitializeCriticalSection(&m_Lock);
}

PciBusDevice::~PciBusDevice()
{
    for (auto &Device : m_Slot)
    {
        for (IVmPciConfigAccessHandler *&Function : Device)
        {
            if (Function != nullptr)
                Function->Release();
        }
    }

    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP PciBusDevice::QueryInterface(REFIID Interface, void **Object)
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
    else if (IsEqualIID(Interface, IID_IVmPciBusService))
    {
        *Object = static_cast<IVmPciBusService *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP PciBusDevice::GetDependencies(void *Repository, ULONG *Count,
                                           GUID **Services, ULONG *Optional)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices
    };

    UNREFERENCED_PARAMETER(Repository);

    return PublishDependencies(Wanted, ARRAYSIZE(Wanted),
                               Count, Services, Optional);
}

/*
 * Only the pair the bus is asked through.
 *
 * The reference answers the port that says whether anything past the first
 * megabyte is reachable from here as well, because on that board this is the
 * part of it that does. On this one the chipset has always answered it, and a
 * board where two things answer the same port is a board where one of them
 * never hears the question.
 */
STDMETHODIMP PciBusDevice::StartReservingResources()
{
    return ReservePorts(PCI_BUS_FIRST_PORT, PCI_BUS_LAST_PORT, this);
}

STDMETHODIMP PciBusDevice::Reset()
{
    EnterCriticalSection(&m_Lock);
    m_Address = 0;
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

/* WHERE A DEVICE ASKS FOR A PLACE ********************************************/

/**
 * @brief
 * Puts a device at one place on the bus, so that questions about that place
 * reach it.
 *
 * @remarks
 * Nothing comes back through Installed. What that would be is how a device
 * raises the line belonging to its place, and no device here has one: the only
 * one that raises a line at all is wired the older way, straight to a number
 * the system has always known it by.
 */
STDMETHODIMP PciBusDevice::InstallPciDevice(IVmPciConfigAccessHandler *Handler,
                                            UCHAR Device, UCHAR Function,
                                            IVmInstalledPciDevice **Installed)
{
    if (Installed != nullptr)
        *Installed = nullptr;

    if (Handler == nullptr)
        return E_POINTER;

    if ((Device >= PCI_BUS_DEVICES) || (Function >= PCI_BUS_FUNCTIONS))
        return E_INVALIDARG;

    /* The places that are the board itself are not ones to be asked for */
    if (IsBoard(Device, Function))
        return E_ACCESSDENIED;

    EnterCriticalSection(&m_Lock);

    const bool Taken = (m_Slot[Device][Function] != nullptr);

    if (!Taken)
    {
        Handler->AddRef();
        m_Slot[Device][Function] = Handler;
    }

    LeaveCriticalSection(&m_Lock);

    return Taken ? E_ACCESSDENIED : S_OK;
}

/* ASKING A DEVICE WHAT IT IS *************************************************/

bool PciBusDevice::IsBoard(UCHAR Device, UCHAR Function)
{
    if ((Device == PCI_BRIDGE_DEVICE) && (Function == PCI_BRIDGE_FUNCTION))
        return true;

    return (Device == PCI_LEGACY_DEVICE) && (Function == PCI_LEGACY_FUNCTION);
}

ULONG PciBusDevice::Bridge(USHORT Offset)
{
    switch (Offset)
    {
        case PCI_WHO_IT_IS:
            return BRIDGE_WHO_IT_IS;

        case PCI_WHAT_IT_IS_DOING:
            return BRIDGE_WHAT_IT_IS_DOING;

        case PCI_WHAT_IT_IS:
            return BRIDGE_WHAT_IT_IS;

        default:
            break;
    }

    /*
     * Everything else is nothing. A way through has no addresses of its own to
     * be given, so there is no place to write one and nothing to read back.
     */
    return 0;
}

ULONG PciBusDevice::Legacy(USHORT Offset)
{
    switch (Offset)
    {
        case PCI_WHO_IT_IS:
            return LEGACY_WHO_IT_IS;

        case PCI_WHAT_IT_IS_DOING:
            return LEGACY_WHAT_IT_IS_DOING;

        case PCI_WHAT_IT_IS:
            return LEGACY_WHAT_IT_IS;

        case PCI_HOW_IT_IS_LAID_OUT:
            return LEGACY_HAS_MORE;

        default:
            break;
    }

    return 0;
}

bool PciBusDevice::Aimed(UCHAR &Device, UCHAR &Function, USHORT &Offset) const
{
    if ((m_Address & PCI_ADDRESS_ENABLED) == 0)
        return false;

    /* One bus, so anything asking about another is asking about nothing */
    if (((m_Address >> 16) & 0xFF) != 0)
        return false;

    Device = (UCHAR)((m_Address >> 11) & 0x1F);
    Function = (UCHAR)((m_Address >> 8) & 0x07);
    Offset = (USHORT)(m_Address & PCI_ADDRESS_REGISTER);

    return Device < PCI_BUS_DEVICES;
}

ULONG PciBusDevice::Ask(UCHAR Device, UCHAR Function, USHORT Offset)
{
    if (IsBoard(Device, Function))
    {
        return (Device == PCI_BRIDGE_DEVICE) ? Bridge(Offset) : Legacy(Offset);
    }

    IVmPciConfigAccessHandler *Handler = m_Slot[Device][Function];

    if (Handler == nullptr)
        return PCI_NOTHING_THERE;

    ULONG Value = 0;

    if (FAILED(Handler->NotifyPciConfigAccess(0, Device, Function, Offset,
                                              FALSE, &Value)))
    {
        return PCI_NOTHING_THERE;
    }

    return Value;
}

void PciBusDevice::Tell(UCHAR Device, UCHAR Function, USHORT Offset,
                        ULONG Value)
{
    if (IsBoard(Device, Function))
        return;

    IVmPciConfigAccessHandler *Handler = m_Slot[Device][Function];

    if (Handler != nullptr)
        Handler->NotifyPciConfigAccess(0, Device, Function, Offset, TRUE, &Value);
}

/* THE TWO PORTS **************************************************************/

/*
 * Which accesses reach the four bytes being carried. A whole one has to be at
 * the first of the four ports, a half of one at either half, and a single byte
 * anywhere. Anything else falls between two of them and reaches nothing.
 */
bool PciBusDevice::Addressed(USHORT Port, ULONG Width)
{
    if ((Port & ~3) != VDEV_PCI_DATA_PORT)
        return false;

    if (Width == 4)
        return Port == VDEV_PCI_DATA_PORT;

    if (Width == 2)
        return (Port & 1) == 0;

    return Width == 1;
}

STDMETHODIMP PciBusDevice::NotifyIoPortRead(USHORT Port, ULONG Width,
                                            ULONG *Value)
{
    if (Value == nullptr)
        return E_POINTER;

    *Value = PCI_NOTHING_THERE;

    EnterCriticalSection(&m_Lock);

    if ((Port == VDEV_PCI_ADDRESS_PORT) && (Width == 4))
    {
        *Value = m_Address;
    }
    else if ((Port == VDEV_PCI_RESET_PORT) && (Width == 1))
    {
        /* Nothing is being asked for, so nothing is owed */
        *Value = 0;
    }
    else if (Addressed(Port, Width))
    {
        UCHAR Device = 0;
        UCHAR Function = 0;
        USHORT Offset = 0;

        if (Aimed(Device, Function, Offset))
            *Value = Ask(Device, Function, Offset);

        /* Which of the four bytes was asked for is in the port, not the address */
        if (Width != 4)
        {
            *Value >>= 8 * (Port & 3);
            *Value &= (Width == 2) ? 0xFFFF : 0xFF;
        }
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP PciBusDevice::NotifyIoPortWrite(USHORT Port, ULONG Width,
                                             ULONG Value)
{
    EnterCriticalSection(&m_Lock);

    if (Port == VDEV_PCI_RESET_PORT)
    {
        /*
         * A guest with nothing left to try asks the board to start again from
         * here. There is nowhere for that to go yet, so the write is taken and
         * the machine keeps running.
         */
    }
    else if (Port == VDEV_PCI_ADDRESS_PORT)
    {
        if (Width == 4)
            m_Address = Value & PCI_ADDRESS_KEPT;
        else if (Width == 2)
            m_Address = (m_Address & 0xFFFF0000) | (Value & 0xFFFC);
        else if (Width == 1)
            m_Address = (m_Address & 0xFFFFFF00) | (Value & 0xFC);
    }
    else if (Addressed(Port, Width))
    {
        UCHAR Device = 0;
        UCHAR Function = 0;
        USHORT Offset = 0;

        if (Aimed(Device, Function, Offset))
        {
            /*
             * A device is only ever given whole registers. Anything narrower
             * is put back into the one it lands in first, which is what the
             * wire does and what keeps a device from having to take one apart.
             */
            if (Width != 4)
            {
                const ULONG Shift = 8 * (Port & 3);
                const ULONG Mask = ((Width == 2) ? 0xFFFFu : 0xFFu) << Shift;

                Value = (Ask(Device, Function, Offset) & ~Mask) |
                        ((Value << Shift) & Mask);
            }

            Tell(Device, Function, Offset, Value);
        }
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

} /* namespace rtvm */
