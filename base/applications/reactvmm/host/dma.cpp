/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The pair of transfer controllers a PC has always had
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Two chips of four channels each, the second addressing words rather than
 * bytes. A device that moves data without the processor programs a channel
 * through these ports and then says it is ready; the controller is what knows
 * where in memory the data is going, because the device never sees an address.
 *
 * Like the interrupt controller this is the manager's own. More than one device
 * shares it, so it cannot belong to any of them.
 */

#include "rtvmm.h"

#include <string.h>

namespace rtvm
{

/* The first chip answers at the bottom of the address space, one port per register */
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
#define DMA_MODE_DIRECTION  0x0C
#define DMA_MODE_TO_MEMORY  0x04
#define DMA_MODE_FROM_MEMORY 0x08
#define DMA_MODE_AUTOINIT   0x10
#define DMA_MODE_BACKWARDS  0x20

/*
 * Where each channel keeps the top of its address. The order is the board's,
 * not the chip's, and there is no pattern in it to work out.
 */
static const USHORT DmaPagePort[8] =
{
    0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A
};

void Dma::Reset()
{
    memset(m_Channel, 0, sizeof(m_Channel));
    memset(m_Chip, 0, sizeof(m_Chip));

    /* A channel nothing has programmed is one nothing may transfer on */
    for (ULONG Index = 0; Index < 8; Index++)
        m_Channel[Index].Masked = true;
}

bool Dma::Owns(USHORT Port) noexcept
{
    return ((Port <= DMA_FIRST_LAST) ||
            ((Port >= DMA_PAGE_BASE) && (Port <= DMA_PAGE_LAST)) ||
            ((Port >= DMA_SECOND_BASE) && (Port <= DMA_SECOND_LAST)));
}

/**
 * @brief
 * Turns a port into which chip it is on and which of its registers it is.
 *
 * @remarks
 * The second chip is wired with a gap between its registers, so a port there
 * names half as many registers as the address range suggests.
 */
bool Dma::Decode(USHORT Port, ULONG &Which, ULONG &Register) const
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

ULONG Dma::ReadPort(USHORT Port)
{
    ULONG Which = 0;
    ULONG Register = 0;

    if ((Port >= DMA_PAGE_BASE) && (Port <= DMA_PAGE_LAST))
    {
        for (ULONG Index = 0; Index < 8; Index++)
        {
            if (DmaPagePort[Index] == Port)
                return m_Channel[Index].Page;
        }

        /* One of the spare latches, which hold whatever was put in them */
        return 0xFF;
    }

    if (!Decode(Port, Which, Register))
        return 0xFF;

    if (Register < DMA_COMMAND)
    {
        const ULONG Channel = (Which * 4) + (Register / 2);
        const bool High = m_Chip[Which].HighByte;
        const USHORT Value = ((Register & 1) != 0)
                           ? m_Channel[Channel].Count
                           : m_Channel[Channel].Address;

        m_Chip[Which].HighByte = !High;
        return High ? (Value >> 8) : (Value & 0xFF);
    }

    if (Register == DMA_COMMAND)
    {
        /*
         * Reading here says which channels have run out and which are asking.
         * The four that have run out are cleared by being read, which is how
         * a driver finds out a transfer finished without being told twice.
         */
        const ULONG Status = m_Chip[Which].Reached | (m_Chip[Which].Asking << 4);

        m_Chip[Which].Reached = 0;
        return Status;
    }

    return 0xFF;
}

void Dma::WritePort(USHORT Port, ULONG Value)
{
    ULONG Which = 0;
    ULONG Register = 0;

    if ((Port >= DMA_PAGE_BASE) && (Port <= DMA_PAGE_LAST))
    {
        for (ULONG Index = 0; Index < 8; Index++)
        {
            if (DmaPagePort[Index] == Port)
            {
                m_Channel[Index].Page = (UCHAR)Value;
                return;
            }
        }

        return;
    }

    if (!Decode(Port, Which, Register))
        return;

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

        return;
    }

    switch (Register)
    {
        case DMA_COMMAND:
            m_Chip[Which].Command = (UCHAR)Value;
            break;

        case DMA_REQUEST:
        {
            const ULONG Channel = (Which * 4) + (Value & 3);

            if ((Value & 0x04) != 0)
                m_Chip[Which].Asking |= (UCHAR)(1 << (Channel & 3));
            else
                m_Chip[Which].Asking &= (UCHAR)~(1 << (Channel & 3));

            break;
        }

        case DMA_SINGLE_MASK:
        {
            const ULONG Channel = (Which * 4) + (Value & 3);

            m_Channel[Channel].Masked = ((Value & 0x04) != 0);
            break;
        }

        case DMA_MODE:
        {
            const ULONG Channel = (Which * 4) + (Value & 3);

            m_Channel[Channel].Mode = (UCHAR)Value;
            break;
        }

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
}

/**
 * @brief
 * Moves a device's data to or from memory, the way a transfer on this channel
 * would have moved it.
 *
 * @param[in] Channel
 * Which of the eight, as the device was wired.
 *
 * @param[in,out] Buffer
 * The device's end of the transfer. Read from when the channel is programmed
 * to fill memory, written to when it is programmed to empty it.
 *
 * @param[in] Length
 * How much the device has, which may be more than the channel was told to move.
 *
 * @param[out] Moved
 * How much actually went, which is as far as the count reached.
 *
 * @return
 * RtvmOk when anything moved at all.
 *
 * @remarks
 * The whole transfer happens in one call rather than a byte at a time. Nothing
 * in the guest can tell the difference: a device that has asked for a transfer
 * holds its own status busy until the count runs out, and the count runs out
 * here before anything else runs.
 */
RTVM_STATUS Dma::Transfer(Memory &Block, ULONG Channel, void *Buffer,
                          ULONG Length, ULONG &Moved)
{
    Moved = 0;

    if ((Channel >= 8) || (Buffer == nullptr) || (Length == 0))
        return RtvmBadParameter;

    ChannelState &State = m_Channel[Channel];

    if (State.Masked)
        return RtvmNotClaimed;

    const bool Wide = (Channel >= 4);
    const ULONG Direction = State.Mode & DMA_MODE_DIRECTION;

    /* A verify transfer moves nothing, and is not an error */
    if ((Direction != DMA_MODE_TO_MEMORY) && (Direction != DMA_MODE_FROM_MEMORY))
        return RtvmOk;

    /*
     * The address the device never sees. A wide channel counts words, so both
     * halves of it mean twice as much as they say.
     */
    const ULONG64 Base = ((ULONG64)State.Page << 16) |
                         (Wide ? ((ULONG64)State.Address << 1) : State.Address);

    /* The count is one less than the number to move, as the chip was designed */
    ULONG Remaining = (ULONG)State.Count + 1;

    if (Wide)
        Remaining *= 2;

    ULONG Amount = (Length < Remaining) ? Length : Remaining;

    if ((State.Mode & DMA_MODE_BACKWARDS) != 0)
    {
        /* Nothing a floppy or a sound card ever asks for, and not worth faking */
        Log(RtvmLogWarning, "channel %lu wants to count down, which is not done\n",
            Channel);
        return RtvmNotSupported;
    }

    bool Went;

    if (Direction == DMA_MODE_TO_MEMORY)
        Went = Block.Write(Base, Buffer, Amount);
    else
        Went = Block.Read(Base, Buffer, Amount);

    if (!Went)
    {
        Log(RtvmLogWarning, "channel %lu points at %08llx, which is not there\n",
            Channel, Base);
        return RtvmBadParameter;
    }

    Moved = Amount;

    /* Where the next transfer on this channel would carry on from */
    const ULONG Step = Wide ? (Amount / 2) : Amount;

    if (Step > State.Count)
    {
        m_Chip[Channel / 4].Reached |= (UCHAR)(1 << (Channel & 3));

        if ((State.Mode & DMA_MODE_AUTOINIT) != 0)
        {
            State.Address = State.BaseAddress;
            State.Count = State.BaseCount;
        }
        else
        {
            State.Count = 0xFFFF;
            State.Masked = true;
        }
    }
    else
    {
        State.Address = (USHORT)(State.Address + Step);
        State.Count = (USHORT)(State.Count - Step);
    }

    return RtvmOk;
}

} /* namespace rtvm */
