/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The pair of interrupt controllers a PC has always had
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Two chips, the second feeding the first through line two. A device holds a
 * line up, the first chip works out whether anything unmasked is pending and of
 * higher priority than what is already being serviced, and the processor is
 * given the vector when it is willing to take one.
 *
 * This is the manager's own rather than a loadable module. Firmware cannot come
 * up without it, and a machine that cannot deliver an interrupt is not a
 * machine worth starting.
 */

#include "rtvmm.h"

#include <string.h>

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
#define ICW1_EXPECT_ICW4    0x01
#define ICW1_SINGLE         0x02
#define ICW1_INITIALISE     0x10
#define ICW4_AUTO_END       0x02

#define OCW2_END_OF_INTERRUPT 0x20
#define OCW2_SPECIFIC         0x40
#define OCW3_SELECT           0x08
#define OCW3_READ_SERVICE     0x01

void Pic::Reset()
{
    memset(m_Chip, 0, sizeof(m_Chip));

    /* Everything masked until the firmware says which lines it wants */
    m_Chip[0].Mask = 0xFF;
    m_Chip[1].Mask = 0xFF;

    /* Where the firmware puts them, and where it leaves them */
    m_Chip[0].Base = 0x08;
    m_Chip[1].Base = 0x70;
}

/**
 * @brief
 * Follows a device's line, and remembers that it went up.
 *
 * @remarks
 * The request register latches. A device that pulses its line, which is what
 * the interval timer does, is gone again long before anything looks, so a
 * register that only mirrored the wire would show nothing and the interrupt
 * would be lost. The latch is what makes a pulse into something that can be
 * delivered later.
 *
 * The state of the wire itself is kept alongside it, because the two are not
 * the same question: the latch says an interrupt is owed, and the wire says
 * whether the device is still asking. A device that holds its line up wants
 * another interrupt after each one is answered, and one that pulsed does not.
 */
void Pic::SetLine(ULONG Line, bool Asserted)
{
    if (Line > 15)
        return;

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
        UpdateCascade();
}

/**
 * @brief
 * Carries the second chip's answer up to the line it is wired to on the first.
 *
 * @remarks
 * The two are not peers. Only the first one is wired to the processor, and the
 * second reaches it by holding the first one's line two. Without this every
 * line from eight upward is latched and then never looked at, which is every
 * interrupt from the clock and the disk.
 */
void Pic::UpdateCascade()
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

int Pic::HighestPending(const Chip &Chip) const
{
    UCHAR Ready = (UCHAR)(Chip.Request & ~Chip.Mask);

    for (int Index = 0; Index < 8; Index++)
    {
        UCHAR Bit = (UCHAR)(1u << Index);

        /*
         * Anything already being serviced blocks everything at or below its
         * own priority, which is what makes a nested handler work.
         */
        if (Chip.Service & Bit)
            return -1;

        if (Ready & Bit)
            return Index;
    }

    return -1;
}

bool Pic::Pending() const
{
    return HighestPending(m_Chip[0]) >= 0;
}

int Pic::Acknowledge()
{
    int Line = HighestPending(m_Chip[0]);

    if (Line < 0)
        return -1;

    m_Chip[0].Service |= (UCHAR)(1u << Line);

    /*
     * Taking it clears what was owed. If the device is still holding its line
     * up it is owed another straight away, which is what keeps a level like
     * the serial port's being served until the device itself lets go.
     */
    m_Chip[0].Request &= (UCHAR)~(1u << Line);

    if (m_Chip[0].Level & (UCHAR)(1u << Line))
        m_Chip[0].Request |= (UCHAR)(1u << Line);

    if (m_Chip[0].AutoEnd)
        m_Chip[0].Service &= (UCHAR)~(1u << Line);

    if (Line != PIC_CASCADE_LINE)
        return m_Chip[0].Base + Line;

    /* The second chip is what is really asking, so ask it which line */
    int Slave = HighestPending(m_Chip[1]);

    if (Slave < 0)
    {
        /* It changed its mind between raising the line and being asked */
        return m_Chip[0].Base + PIC_CASCADE_LINE;
    }

    m_Chip[1].Service |= (UCHAR)(1u << Slave);
    m_Chip[1].Request &= (UCHAR)~(1u << Slave);

    if (m_Chip[1].Level & (UCHAR)(1u << Slave))
        m_Chip[1].Request |= (UCHAR)(1u << Slave);

    if (m_Chip[1].AutoEnd)
        m_Chip[1].Service &= (UCHAR)~(1u << Slave);

    return m_Chip[1].Base + Slave;
}

ULONG Pic::ReadPort(USHORT Port)
{
    Chip &Chip = m_Chip[((Port & 0x80) != 0) ? 1 : 0];

    if ((Port == PIC_MASTER_COMMAND) || (Port == PIC_SLAVE_COMMAND))
        return Chip.ReadService ? Chip.Service : Chip.Request;

    return Chip.Mask;
}

void Pic::WritePort(USHORT Port, ULONG Value)
{
    Chip &Chip = m_Chip[((Port & 0x80) != 0) ? 1 : 0];
    UCHAR Byte = (UCHAR)(Value & 0xFF);
    bool IsCommand = (Port == PIC_MASTER_COMMAND) || (Port == PIC_SLAVE_COMMAND);

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
            return;
        }

        if (Byte & OCW3_SELECT)
        {
            /* Picking which of the two registers the command port reads back */
            Chip.ReadService = (Byte & OCW3_READ_SERVICE) != 0;
            return;
        }

        if (Byte & OCW2_END_OF_INTERRUPT)
        {
            if (Byte & OCW2_SPECIFIC)
            {
                Chip.Service &= (UCHAR)~(1u << (Byte & 7));
                return;
            }

            /* Otherwise the one being serviced at the highest priority goes */
            for (int Index = 0; Index < 8; Index++)
            {
                UCHAR Bit = (UCHAR)(1u << Index);

                if (Chip.Service & Bit)
                {
                    Chip.Service &= (UCHAR)~Bit;
                    break;
                }
            }
        }

        return;
    }

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
            UpdateCascade();
            break;
    }
}

} /* namespace rtvm */
