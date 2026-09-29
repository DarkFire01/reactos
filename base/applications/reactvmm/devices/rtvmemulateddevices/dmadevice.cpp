/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The pair of transfer controllers a PC has always had
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Two chips of four channels each, the second addressing words rather than
 * bytes. A device programs a channel through these ports and then asks for a
 * turn on it, and what it is given back is an address and a length. The moving
 * is the asking device's to do, which is why nothing here has any way of
 * reaching guest memory at all.
 */

#include "dmadevice.h"

namespace rtvm
{

/* The first chip answers at the bottom of the address space, one port each */
#define DMA_FIRST_BASE      0x00
#define DMA_FIRST_LAST      0x0F

/* The second is spread out at twice the spacing, the way the board wires it */
#define DMA_SECOND_BASE     0xC0
#define DMA_SECOND_LAST     0xDF

/* The high bits of the address, which the chips never had room for */
#define DMA_PAGE_BASE       0x80
#define DMA_PAGE_LAST       0x8F

/* What the registers past the channels do */
#define DMA_COMMAND         0x08
#define DMA_REQUEST         0x09
#define DMA_SINGLE_MASK     0x0A
#define DMA_MODE            0x0B
#define DMA_CLEAR_FLIPFLOP  0x0C
#define DMA_MASTER_CLEAR    0x0D
#define DMA_CLEAR_MASK      0x0E
#define DMA_ALL_MASK        0x0F

/* Which way the mode register says a channel moves data */
#define MODE_DIRECTION      0x0C
#define MODE_TO_MEMORY      0x04
#define MODE_FROM_MEMORY    0x08
#define MODE_AUTOINIT       0x10
#define MODE_BACKWARDS      0x20

/*
 * Where each channel keeps the top of its address. The order is the board's,
 * not the chip's, and there is no pattern in it to work out.
 */
static const USHORT DmaPagePort[8] =
{
    0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A
};

/* WHAT IT IS *****************************************************************/

DmaDevice::DmaDevice()
{
    InitializeCriticalSection(&m_Lock);
    Clear();
}

DmaDevice::~DmaDevice()
{
    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP DmaDevice::QueryInterface(REFIID Interface, void **Object)
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
    else if (IsEqualIID(Interface, IID_IVmDmaController))
    {
        *Object = static_cast<IVmDmaController *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP DmaDevice::GetDependencies(void *Repository, ULONG *Count,
                                        GUID **Services, ULONG *Optional)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IVmTimeSource
    };

    UNREFERENCED_PARAMETER(Repository);

    return PublishDependencies(Wanted, ARRAYSIZE(Wanted),
                               Count, Services, Optional);
}

STDMETHODIMP DmaDevice::StartReservingResources()
{
    HRESULT Status = ReservePorts(DMA_FIRST_BASE, DMA_FIRST_LAST, this);

    if (SUCCEEDED(Status))
        Status = ReservePorts(DMA_PAGE_BASE, DMA_PAGE_LAST, this);

    if (SUCCEEDED(Status))
        Status = ReservePorts(DMA_SECOND_BASE, DMA_SECOND_LAST, this);

    return Status;
}

STDMETHODIMP DmaDevice::PowerOnCold()
{
    return Reset();
}

STDMETHODIMP DmaDevice::Reset()
{
    EnterCriticalSection(&m_Lock);
    Clear();
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

void DmaDevice::Clear()
{
    memset(m_Channel, 0, sizeof(m_Channel));
    memset(m_Chip, 0, sizeof(m_Chip));
    memset(m_Granted, 0, sizeof(m_Granted));

    /* A channel nothing has programmed is one nothing may transfer on */
    for (ULONG Index = 0; Index < 8; Index++)
        m_Channel[Index].Masked = true;
}

/* THE REGISTERS **************************************************************/

/**
 * @brief
 * Turns a port into which chip it is on and which of its registers it is.
 *
 * @remarks
 * The second chip is wired with a gap between its registers, so a port there
 * names half as many registers as the address range suggests.
 */
bool DmaDevice::Decode(USHORT Port, ULONG &Which, ULONG &Register) const
{
    if (Port <= DMA_FIRST_LAST)
    {
        Which = 0;
        Register = Port;
        return true;
    }

    if ((Port >= DMA_SECOND_BASE) && (Port <= DMA_SECOND_LAST))
    {
        Which = 1;
        Register = (ULONG)((Port - DMA_SECOND_BASE) / 2);
        return true;
    }

    return false;
}

STDMETHODIMP DmaDevice::NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value)
{
    ULONG Which = 0;
    ULONG Register = 0;

    UNREFERENCED_PARAMETER(Width);

    if (Value == nullptr)
        return E_POINTER;

    *Value = 0xFF;

    EnterCriticalSection(&m_Lock);

    if ((Port >= DMA_PAGE_BASE) && (Port <= DMA_PAGE_LAST))
    {
        for (ULONG Index = 0; Index < 8; Index++)
        {
            if (DmaPagePort[Index] == Port)
            {
                *Value = m_Channel[Index].Page;
                break;
            }
        }
    }
    else if (Decode(Port, Which, Register))
    {
        if (Register < DMA_COMMAND)
        {
            const ULONG Channel = (Which * 4) + (Register / 2);
            const bool High = m_Chip[Which].HighByte;
            const USHORT Held = ((Register & 1) != 0)
                              ? m_Channel[Channel].Count
                              : m_Channel[Channel].Address;

            m_Chip[Which].HighByte = !High;
            *Value = High ? (Held >> 8) : (Held & 0xFF);
        }
        else if (Register == DMA_COMMAND)
        {
            /*
             * Which channels have run out and which are asking. The four that
             * have run out are cleared by being read, which is how a driver
             * learns a transfer finished without being told twice.
             */
            *Value = m_Chip[Which].Reached | (ULONG)(m_Chip[Which].Asking << 4);
            m_Chip[Which].Reached = 0;
        }
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP DmaDevice::NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value)
{
    ULONG Which = 0;
    ULONG Register = 0;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&m_Lock);

    if ((Port >= DMA_PAGE_BASE) && (Port <= DMA_PAGE_LAST))
    {
        for (ULONG Index = 0; Index < 8; Index++)
        {
            if (DmaPagePort[Index] == Port)
            {
                m_Channel[Index].Page = (UCHAR)Value;
                break;
            }
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    if (!Decode(Port, Which, Register))
    {
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    /* The address and count registers, each written low half first */
    if (Register < DMA_COMMAND)
    {
        const ULONG Channel = (Which * 4) + (Register / 2);
        const bool High = m_Chip[Which].HighByte;
        USHORT &Target = ((Register & 1) != 0)
                       ? m_Channel[Channel].BaseCount
                       : m_Channel[Channel].BaseAddress;

        if (High)
            Target = (USHORT)((Target & 0x00FF) | ((Value & 0xFF) << 8));
        else
            Target = (USHORT)((Target & 0xFF00) | (Value & 0xFF));

        m_Chip[Which].HighByte = !High;

        /* What is programmed is also where the transfer starts from */
        if ((Register & 1) != 0)
            m_Channel[Channel].Count = m_Channel[Channel].BaseCount;
        else
            m_Channel[Channel].Address = m_Channel[Channel].BaseAddress;

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    switch (Register)
    {
        case DMA_COMMAND:
            m_Chip[Which].Command = (UCHAR)Value;
            break;

        case DMA_REQUEST:
            if ((Value & 0x04) != 0)
                m_Chip[Which].Asking |= (UCHAR)(1u << (Value & 3));
            else
                m_Chip[Which].Asking &= (UCHAR)~(1u << (Value & 3));

            break;

        case DMA_SINGLE_MASK:
            m_Channel[(Which * 4) + (Value & 3)].Masked = ((Value & 0x04) != 0);
            break;

        case DMA_MODE:
            m_Channel[(Which * 4) + (Value & 3)].Mode = (UCHAR)Value;
            break;

        case DMA_CLEAR_FLIPFLOP:
            m_Chip[Which].HighByte = false;
            break;

        case DMA_MASTER_CLEAR:
            m_Chip[Which].HighByte = false;
            m_Chip[Which].Command = 0;
            m_Chip[Which].Reached = 0;
            m_Chip[Which].Asking = 0;

            for (ULONG Index = 0; Index < 4; Index++)
                m_Channel[(Which * 4) + Index].Masked = true;

            break;

        case DMA_CLEAR_MASK:
            for (ULONG Index = 0; Index < 4; Index++)
                m_Channel[(Which * 4) + Index].Masked = false;

            break;

        case DMA_ALL_MASK:
            for (ULONG Index = 0; Index < 4; Index++)
            {
                m_Channel[(Which * 4) + Index].Masked =
                    ((Value & (1u << Index)) != 0);
            }

            break;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/* A TURN ON A CHANNEL ********************************************************/

STDMETHODIMP DmaDevice::GetDmaChannelCount(ULONG *Count)
{
    if (Count == nullptr)
        return E_POINTER;

    *Count = ARRAYSIZE(m_Channel);
    return S_OK;
}

/**
 * @brief
 * Gives a device a turn on a channel, as an address and a length.
 *
 * @remarks
 * Nothing moves here. The device that asked knows what it has and where it
 * wants it; what it does not know, and has no way of finding out, is where in
 * memory the transfer is meant to land. That is the one thing this answers.
 *
 * How much it may move is the smaller of what it has and what the channel was
 * programmed for. A driver that made room for one sector and a device holding
 * a whole track is the ordinary case, not an error.
 */
STDMETHODIMP DmaDevice::RequestDma(ULONG Channel, double Unknown, ULONG Length,
                                   ULONG *Direction, ULONG64 *Address,
                                   ULONG *Count, ULONG *Result)
{
    UNREFERENCED_PARAMETER(Unknown);

    if ((Channel >= ARRAYSIZE(m_Channel)) || (Direction == nullptr) ||
        (Address == nullptr) || (Count == nullptr))
    {
        return E_INVALIDARG;
    }

    *Direction = DMA_NEITHER_WAY;
    *Address = 0;
    *Count = 0;

    if (Result != nullptr)
        *Result = DMA_REQUEST_REFUSED;

    EnterCriticalSection(&m_Lock);

    ChannelState &State = m_Channel[Channel];

    if (State.Masked)
    {
        LeaveCriticalSection(&m_Lock);
        return E_ACCESSDENIED;
    }

    if ((State.Mode & MODE_BACKWARDS) != 0)
    {
        /* Nothing a drive or a sound card ever asks for, and not worth faking */
        LeaveCriticalSection(&m_Lock);
        return E_NOTIMPL;
    }

    const bool Wide = (Channel >= 4);
    const ULONG Moving = State.Mode & MODE_DIRECTION;

    if (Moving == MODE_TO_MEMORY)
        *Direction = DMA_TO_MEMORY;
    else if (Moving == MODE_FROM_MEMORY)
        *Direction = DMA_FROM_MEMORY;

    /*
     * A wide channel counts words, so both halves of its address and its count
     * mean twice as much as they say.
     */
    *Address = ((ULONG64)State.Page << 16) |
               (Wide ? ((ULONG64)State.Address << 1) : State.Address);

    /* The count is one less than the number to move, as the chip was designed */
    ULONG Room = (ULONG)State.Count + 1;

    if (Wide)
        Room *= 2;

    *Count = (Length < Room) ? Length : Room;
    m_Granted[Channel] = *Count;

    if (Result != nullptr)
        *Result = DMA_REQUEST_TAKEN;

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/**
 * @brief
 * Told by the device that the data it was given a turn for has been moved.
 *
 * @remarks
 * Only now does the channel move on. Doing it when the turn was handed out
 * would be trusting a device to have carried out a transfer it had not even
 * started, and a device that fails part way through would leave the channel
 * pointing somewhere nothing was written.
 */
STDMETHODIMP DmaDevice::ReportDmaComplete(ULONG Channel)
{
    if (Channel >= ARRAYSIZE(m_Channel))
        return E_INVALIDARG;

    EnterCriticalSection(&m_Lock);
    Advance(Channel, m_Granted[Channel]);
    m_Granted[Channel] = 0;
    LeaveCriticalSection(&m_Lock);

    return S_OK;
}

void DmaDevice::Advance(ULONG Channel, ULONG Moved)
{
    ChannelState &State = m_Channel[Channel];
    const bool Wide = (Channel >= 4);
    const ULONG Step = Wide ? (Moved / 2) : Moved;

    if (Moved == 0)
        return;

    if (Step > State.Count)
    {
        m_Chip[Channel / 4].Reached |= (UCHAR)(1u << (Channel & 3));

        if ((State.Mode & MODE_AUTOINIT) != 0)
        {
            State.Address = State.BaseAddress;
            State.Count = State.BaseCount;
        }
        else
        {
            State.Count = 0xFFFF;
            State.Masked = true;
        }

        return;
    }

    State.Address = (USHORT)(State.Address + Step);
    State.Count = (USHORT)(State.Count - Step);
}

} /* namespace rtvm */
