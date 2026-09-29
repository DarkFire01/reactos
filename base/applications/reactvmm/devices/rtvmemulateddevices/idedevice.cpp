/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The controller the disks and the optical drive hang off
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * One controller, two channels, two drives on each. A drive is not a device of
 * its own: what the guest talks to is the controller, and which of the four it
 * means is a bit in one of the registers.
 *
 * A drive is either told what to do by writing registers or by handing it a
 * whole command, and the two kinds sit side by side on the same wires. That is
 * how an optical drive and a disk have always shared a cable.
 */

#include "idedevice.h"

#include <stdio.h>
#include <stdlib.h>

namespace rtvm
{

/* Offsets from the base of a channel */
#define IDE_DATA                0
#define IDE_ERROR               1   /* Read */
#define IDE_FEATURES            1   /* Write */
#define IDE_COUNT               2
#define IDE_LBA_LOW             3
#define IDE_LBA_MID             4
#define IDE_LBA_HIGH            5
#define IDE_SELECT              6
#define IDE_STATUS              7   /* Read */
#define IDE_COMMAND             7   /* Write */

/* What the status register says */
#define STATUS_ERROR            0x01
#define STATUS_DATA             0x08
#define STATUS_SEEK_DONE        0x10
#define STATUS_FAULT            0x20
#define STATUS_READY            0x40
#define STATUS_BUSY             0x80

/* What the error register says */
#define ERROR_ABORTED           0x04
#define ERROR_NO_SUCH_SECTOR    0x10
#define ERROR_UNREADABLE        0x40

/* Which drive, and whether the place is counted the old way or the new */
#define SELECT_DRIVE            0x10
#define SELECT_LBA              0x40
#define SELECT_HEAD             0x0F

/* What the register kept apart from the others does */
#define DEVICE_NO_INTERRUPT     0x02
#define DEVICE_RESET            0x04

/* The commands a drive answers */
#define COMMAND_RECALIBRATE     0x10
#define COMMAND_READ_SECTORS    0x20
#define COMMAND_READ_NO_RETRY   0x21
#define COMMAND_WRITE_SECTORS   0x30
#define COMMAND_WRITE_NO_RETRY  0x31
#define COMMAND_PACKET          0xA0
#define COMMAND_IDENTIFY_PACKET 0xA1
#define COMMAND_SET_PARAMETERS  0x91
#define COMMAND_READ_MULTIPLE   0xC4
#define COMMAND_WRITE_MULTIPLE  0xC5
#define COMMAND_SET_MULTIPLE    0xC6
#define COMMAND_FLUSH           0xE7
#define COMMAND_IDENTIFY        0xEC

/*
 * What the register that counts sectors carries on a drive told in whole
 * commands. It has no sectors to count there, so it says instead which way
 * the next run of bytes is going and whether that run is the command itself.
 * Anything driving one of these reads it at every stop and will not carry on
 * until it agrees with what the drive was asked for.
 */
#define REASON_COMMAND          0x01
#define REASON_TO_HOST          0x02

/* The most a drive hands over at once when it was not told a smaller number */
#define REASON_ANY_AMOUNT       0xFFFE

/* The commands inside a whole one, of which only these are ever sent */
#define PACKET_TEST_UNIT_READY  0x00
#define PACKET_REQUEST_SENSE    0x03
#define PACKET_INQUIRY          0x12
#define PACKET_READ_CAPACITY    0x25
#define PACKET_READ_10          0x28

/* The shape a disk is described as having when nothing can ask it */
#define SHAPE_HEADS             16
#define SHAPE_SECTORS           63

/* WHAT IT IS *****************************************************************/

IdeControllerDevice::IdeControllerDevice()
{
    InitializeCriticalSection(&m_Lock);

    m_Channel[0].Base = IDE_PRIMARY_BASE;
    m_Channel[0].Control = IDE_PRIMARY_CONTROL;
    m_Channel[0].Line = IDE_PRIMARY_LINE;

    m_Channel[1].Base = IDE_SECONDARY_BASE;
    m_Channel[1].Control = IDE_SECONDARY_CONTROL;
    m_Channel[1].Line = IDE_SECONDARY_LINE;

    m_Doing = IDE_DOING_AT_REST;

    /*
     * The lowest bit of where the part that moves data answers is not a bit of
     * an address: it says that what follows is a run of ports rather than a
     * run of memory, and it is wired that way. Starting it at nothing reads as
     * a run of memory, and whatever is walking the bus then works out one kind
     * from how wide the run is and the other from this, disagrees with itself,
     * and stops.
     */
    m_MoverPorts = IDE_MOVER_IS_PORTS;

    for (Channel &One : m_Channel)
    {
        One.Status = STATUS_READY | STATUS_SEEK_DONE;

        for (Drive &What : One.Drives)
            What.Image = INVALID_HANDLE_VALUE;
    }
}

IdeControllerDevice::~IdeControllerDevice()
{
    for (Channel &One : m_Channel)
    {
        for (Drive &What : One.Drives)
        {
            if (What.Image != INVALID_HANDLE_VALUE)
                CloseHandle(What.Image);
        }
    }

    if (m_Lines != nullptr)
        m_Lines->Release();

    DeleteCriticalSection(&m_Lock);
}

STDMETHODIMP IdeControllerDevice::QueryInterface(REFIID Interface, void **Object)
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
    else if (IsEqualIID(Interface, IID_IVmPciConfigAccessHandler))
    {
        *Object = static_cast<IVmPciConfigAccessHandler *>(this);
    }
    else if (IsEqualIID(Interface, IID_IRtvmDeviceSettings))
    {
        *Object = static_cast<IRtvmDeviceSettings *>(this);
    }
    else
    {
        *Object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP IdeControllerDevice::GetDependencies(void *Repository, ULONG *Count,
                                                  GUID **Services, ULONG *Optional)
{
    static const GUID *const Wanted[] =
    {
        &IID_IVmAmd64EmulationServices,
        &IID_IVmIoApic,
        &IID_IVmPciBusService
    };

    UNREFERENCED_PARAMETER(Repository);

    return PublishDependencies(Wanted, ARRAYSIZE(Wanted),
                               Count, Services, Optional);
}

STDMETHODIMP IdeControllerDevice::StartReservingResources()
{
    for (Channel &One : m_Channel)
    {
        bool Anything = false;

        for (const Drive &What : One.Drives)
            Anything = Anything || What.Present;

        /* A channel with nothing on it answers for nothing */
        if (!Anything)
            continue;

        HRESULT Status = ReservePorts(One.Base,
                                      One.Base + IDE_REGISTER_COUNT - 1, this);

        if (SUCCEEDED(Status))
            Status = ReservePorts(One.Control, One.Control, this);

        if (FAILED(Status))
            return Status;
    }

    /*
     * And a place on the bus, so that a system which will not look at an
     * address it was not told about finds this controller there. The registers
     * above stay where they have always been: a controller in this mode is
     * found by asking, and then driven at the addresses it always had.
     */
    IVmPciBusService *Bus = nullptr;

    if (SUCCEEDED(FindService(IID_IVmPciBusService,
                              reinterpret_cast<void **>(&Bus))))
    {
        Bus->InstallPciDevice(this, IDE_BUS_DEVICE, IDE_BUS_FUNCTION, nullptr);
        Bus->Release();
    }

    return S_OK;
}

STDMETHODIMP IdeControllerDevice::PowerOnCold()
{
    if (m_Lines == nullptr)
        FindService(IID_IVmIoApic, reinterpret_cast<void **>(&m_Lines));

    return Reset();
}

STDMETHODIMP IdeControllerDevice::PowerOff()
{
    for (Channel &One : m_Channel)
        SetLine(One, false);

    return S_OK;
}

STDMETHODIMP IdeControllerDevice::Reset()
{
    EnterCriticalSection(&m_Lock);

    for (Channel &One : m_Channel)
    {
        One.Status = STATUS_READY | STATUS_SEEK_DONE;
        One.Error = 0;
        One.Select = 0;
        One.Device = 0;
        One.Length = 0;
        One.Offset = 0;
        One.Writing = false;
        One.Expecting = false;
        One.CommandLength = 0;
        One.Remaining = 0;
        One.PacketTotal = 0;
        One.PacketLimit = 0;
    }

    m_Doing = IDE_DOING_AT_REST;
    m_MoverPorts = IDE_MOVER_IS_PORTS;
    m_Timing[0] = 0;
    m_Timing[1] = 0;
    m_Timing[2] = 0;

    for (ULONG Which = 0; Which < 2; Which++)
    {
        m_MoverCommand[Which] = 0;
        m_MoverStatus[Which] = 0;
        m_MoverList[Which] = 0;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/* WHAT IT SAYS IT IS *********************************************************/

/*
 * The part of a controller of this kind that moves data without asking the
 * processor for every word. Its registers are here because the description
 * says the controller has them and a system will read them either way; nothing
 * ever starts it, because no drive on this controller says it can be moved
 * from that way and so nothing asks. A guest that asked anyway is told the
 * transfer failed rather than being left waiting for one that will not happen.
 */
bool IdeControllerDevice::Moving(USHORT Port, ULONG &Which,
                                 ULONG &Register) const
{
    const USHORT Base = (USHORT)(m_MoverPorts & 0xFFF0);

    if (!m_MoverPlaced || (Base == 0))
        return false;

    if ((Port < Base) || (Port >= (Base + IDE_MOVER_LENGTH)))
        return false;

    const ULONG Offset = (ULONG)(Port - Base);

    Which = Offset / IDE_MOVER_CHANNEL;
    Register = Offset % IDE_MOVER_CHANNEL;
    return true;
}

void IdeControllerDevice::PlaceMover()
{
    const USHORT Base = (USHORT)(m_MoverPorts & 0xFFF0);

    if (m_MoverPlaced || (Base == 0) || ((m_Doing & IDE_DECODES_PORTS) == 0))
        return;

    if (SUCCEEDED(ReservePorts(Base, (USHORT)(Base + IDE_MOVER_LENGTH - 1), this)))
        m_MoverPlaced = true;
}

UCHAR IdeControllerDevice::MoverRead(ULONG Which, ULONG Register) const
{
    if (Register == IDE_MOVER_COMMAND)
        return m_MoverCommand[Which];

    if (Register == IDE_MOVER_STATUS)
        return m_MoverStatus[Which];

    if (Register >= IDE_MOVER_LIST)
    {
        const ULONG Shift = 8 * (Register - IDE_MOVER_LIST);

        return (UCHAR)((m_MoverList[Which] >> Shift) & 0xFF);
    }

    return 0;
}

void IdeControllerDevice::MoverWrite(ULONG Which, ULONG Register, UCHAR Byte)
{
    if (Register == IDE_MOVER_COMMAND)
    {
        m_MoverCommand[Which] = (UCHAR)(Byte & IDE_MOVER_DIRECTION);

        /* Asked to run, and there is nothing here that runs */
        if ((Byte & IDE_MOVER_STARTED) != 0)
        {
            m_MoverStatus[Which] =
                (UCHAR)((m_MoverStatus[Which] & ~IDE_MOVER_RUNNING) |
                        IDE_MOVER_FAILED);
        }

        return;
    }

    if (Register == IDE_MOVER_STATUS)
    {
        /* The two that say what happened are put out by writing them back */
        m_MoverStatus[Which] &=
            (UCHAR)~(Byte & (IDE_MOVER_FAILED | IDE_MOVER_FINISHED));
        return;
    }

    if (Register >= IDE_MOVER_LIST)
    {
        const ULONG Shift = 8 * (Register - IDE_MOVER_LIST);
        const ULONG Mask = 0xFFul << Shift;

        m_MoverList[Which] = (m_MoverList[Which] & ~Mask) |
                             (((ULONG)Byte << Shift) & Mask);

        /* A list of what to move is always on a four byte boundary */
        m_MoverList[Which] &= 0xFFFFFFFC;
    }
}

STDMETHODIMP IdeControllerDevice::NotifyPciConfigAccess(UCHAR Bus, UCHAR Device,
                                                        UCHAR Function,
                                                        USHORT Offset,
                                                        UCHAR Writing,
                                                        ULONG *Value)
{
    UNREFERENCED_PARAMETER(Bus);
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Function);

    if (Value == nullptr)
        return E_POINTER;

    EnterCriticalSection(&m_Lock);

    if (Writing)
    {
        switch (Offset)
        {
            case IDE_PCI_DOING:
                /*
                 * The two bits that say whether it answers at all and whether
                 * it may move data of its own accord are the guest's. The ones
                 * above them say what has gone wrong and are put out by being
                 * written back.
                 */
                m_Doing &= ~(*Value & IDE_DOING_CLEARED);
                m_Doing = (m_Doing & ~IDE_DOING_KEPT) |
                          (*Value & IDE_DOING_KEPT);
                PlaceMover();
                break;

            case IDE_PCI_MOVER:
                m_MoverPorts = (*Value & 0xFFF0) | IDE_MOVER_IS_PORTS;
                PlaceMover();
                break;

            case IDE_PCI_TIMING:
                m_Timing[0] = *Value;
                break;

            case IDE_PCI_TIMING + 4:
                m_Timing[1] = *Value;
                break;

            case IDE_PCI_TIMING + 8:
                m_Timing[2] = *Value;
                break;

            default:
                break;
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    switch (Offset)
    {
        case IDE_PCI_WHO:
            *Value = IDE_WHO_IT_IS;
            break;

        case IDE_PCI_DOING:
            *Value = m_Doing;
            break;

        case IDE_PCI_WHAT:
            *Value = IDE_WHAT_IT_IS;
            break;

        case IDE_PCI_MOVER:
            *Value = m_MoverPorts;
            break;

        case IDE_PCI_TIMING:
            *Value = m_Timing[0];
            break;

        case IDE_PCI_TIMING + 4:
            *Value = m_Timing[1];
            break;

        case IDE_PCI_TIMING + 8:
            *Value = m_Timing[2];
            break;

        default:
            /*
             * Everything else, the place where a line would be named among it.
             * A controller left in the older mode has none: both channels
             * raise the two numbers they have always raised, and a system that
             * read a line here would go looking for interrupts that never come.
             */
            *Value = 0;
            break;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/* WHICH DRIVE ****************************************************************/

IdeControllerDevice::Channel *
IdeControllerDevice::Find(USHORT Port, ULONG *Register, bool *IsControl)
{
    for (Channel &One : m_Channel)
    {
        if (Port == One.Control)
        {
            *Register = 0;
            *IsControl = true;
            return &One;
        }

        if ((Port >= One.Base) && (Port < (One.Base + IDE_REGISTER_COUNT)))
        {
            *Register = (ULONG)(Port - One.Base);
            *IsControl = false;
            return &One;
        }
    }

    return nullptr;
}

IdeControllerDevice::Drive *IdeControllerDevice::Selected(Channel &On)
{
    Drive &What = On.Drives[(On.Select & SELECT_DRIVE) ? 1 : 0];

    return What.Present ? &What : nullptr;
}

void IdeControllerDevice::SetLine(Channel &On, bool Asserted)
{
    /* A caller that turned interrupts off on this channel gets none */
    if (Asserted && ((On.Device & DEVICE_NO_INTERRUPT) != 0))
        return;

    if (Asserted == On.LineAsserted)
    {
        /*
         * Held already, and what carries it latches on an edge alone. Letting
         * it go and raising it again is what makes one, and is what a run of
         * sectors coming back one at a time needs.
         */
        if (!Asserted)
            return;

        if (m_Lines != nullptr)
            m_Lines->DeassertIrq(On.Line);

        On.LineAsserted = false;
    }

    On.LineAsserted = Asserted;

    if (m_Lines == nullptr)
        return;

    if (Asserted)
        m_Lines->AssertIrq(On.Line);
    else
        m_Lines->DeassertIrq(On.Line);
}

void IdeControllerDevice::Fail(Channel &On, UCHAR Why)
{
    const Drive *What = Selected(On);

    On.Error = Why;
    On.Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_ERROR;
    On.Length = 0;
    On.Offset = 0;
    On.Remaining = 0;
    On.Writing = false;

    /* A drive told in whole commands says a failed one is over the same way */
    if ((What != nullptr) && What->Packet)
    {
        On.PacketTotal = 0;
        On.Count = REASON_COMMAND | REASON_TO_HOST;
    }

    SetLine(On, true);
}

void IdeControllerDevice::Ready(Channel &On)
{
    On.Error = 0;
    On.Status = STATUS_READY | STATUS_SEEK_DONE;
    On.Length = 0;
    On.Offset = 0;
    On.Remaining = 0;
    On.Writing = false;

    SetLine(On, true);
}

/* WHAT A DRIVE SAYS IT IS ****************************************************/

static void IdeText(UCHAR *Where, const char *Text, ULONG Length)
{
    /*
     * Two characters per word, the second before the first. Everything that
     * reads one of these swaps them back, and one written the other way round
     * reads as gibberish.
     */
    for (ULONG Index = 0; Index < Length; Index += 2)
    {
        const char First = (Text[Index] != '\0') ? Text[Index] : ' ';
        const char Second = (First != ' ') && (Text[Index + 1] != '\0')
                          ? Text[Index + 1]
                          : ' ';

        Where[Index] = (UCHAR)Second;
        Where[Index + 1] = (UCHAR)First;
    }
}

void IdeControllerDevice::Identify(Channel &On, Drive &What)
{
    USHORT *Words = reinterpret_cast<USHORT *>(On.Buffer);

    memset(On.Buffer, 0, IDE_SECTOR_SIZE);

    if (What.Packet)
    {
        /*
         * A drive told in whole commands says so in the top bits, along with
         * how long one of those is. Nothing below about a shape means anything
         * to it, because it has none a caller may address it by.
         */
        Words[0] = 0x85C0;

        IdeText(&On.Buffer[20], "0", 20);
        IdeText(&On.Buffer[46], "1.0", 8);
        IdeText(&On.Buffer[54], "ReacTVmm optical drive", 40);

        Words[49] = 0x0200;
        Words[53] = 0x0006;
    }
    else
    {
        Words[0] = 0x0040;
        Words[1] = (USHORT)What.Cylinders;
        Words[3] = (USHORT)What.Heads;
        Words[6] = (USHORT)What.Sectors;

        IdeText(&On.Buffer[20], "0", 20);
        IdeText(&On.Buffer[46], "1.0", 8);
        IdeText(&On.Buffer[54], "ReacTVmm disk", 40);

        Words[47] = 0x8001;
        Words[49] = 0x0200;
        Words[53] = 0x0007;
        Words[54] = (USHORT)What.Cylinders;
        Words[55] = (USHORT)What.Heads;
        Words[56] = (USHORT)What.Sectors;

        const ULONG Addressable = (What.SectorCount > 0x0FFFFFFF)
                                ? 0x0FFFFFFF
                                : (ULONG)What.SectorCount;

        Words[57] = (USHORT)(Addressable & 0xFFFF);
        Words[58] = (USHORT)(Addressable >> 16);
        Words[60] = (USHORT)(Addressable & 0xFFFF);
        Words[61] = (USHORT)(Addressable >> 16);
    }

    On.Length = IDE_SECTOR_SIZE;
    On.Offset = 0;
    On.Writing = false;
    On.Error = 0;
    On.Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_DATA;

    SetLine(On, true);
}

/* READING AND WRITING ********************************************************/

ULONG64 IdeControllerDevice::Place(Channel &On, Drive &What) const
{
    if ((On.Select & SELECT_LBA) != 0)
    {
        return ((ULONG64)(On.Select & SELECT_HEAD) << 24) |
               ((ULONG64)On.LbaHigh << 16) |
               ((ULONG64)On.LbaMid << 8) |
               (ULONG64)On.LbaLow;
    }

    const ULONG Cylinder = ((ULONG)On.LbaHigh << 8) | On.LbaMid;
    const ULONG Head = On.Select & SELECT_HEAD;
    const ULONG Sector = On.LbaLow;

    /* Sectors have always been counted from one, unlike everything else */
    if (Sector == 0)
        return (ULONG64)-1;

    return ((ULONG64)Cylinder * What.Heads + Head) * What.Sectors + (Sector - 1);
}

void IdeControllerDevice::Begin(Channel &On, bool Writing)
{
    Drive *What = Selected(On);

    if (What == nullptr)
    {
        Fail(On, ERROR_ABORTED);
        return;
    }

    if (Writing && What->ReadOnly)
    {
        Fail(On, ERROR_ABORTED);
        return;
    }

    const ULONG64 First = Place(On, *What);
    ULONG Count = (On.Count == 0) ? 256 : On.Count;

    if ((First == (ULONG64)-1) || ((First + Count) > What->SectorCount))
    {
        Fail(On, ERROR_NO_SUCH_SECTOR);
        return;
    }

    if ((Count * IDE_SECTOR_SIZE) > sizeof(On.Buffer))
        Count = sizeof(On.Buffer) / IDE_SECTOR_SIZE;

    On.Length = Count * IDE_SECTOR_SIZE;
    On.Offset = 0;
    On.Writing = Writing;
    On.Next = First;
    On.Error = 0;

    if (Writing)
    {
        /* Nothing goes out until the whole run has been handed over */
        On.Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_DATA;
        return;
    }

    LARGE_INTEGER Where;
    DWORD Read = 0;

    Where.QuadPart = (LONGLONG)(First * IDE_SECTOR_SIZE);
    SetFilePointerEx(What->Image, Where, nullptr, FILE_BEGIN);

    if (!ReadFile(What->Image, On.Buffer, On.Length, &Read, nullptr))
    {
        Fail(On, ERROR_UNREADABLE);
        return;
    }

    /* Past the end of a short image reads as an empty sector would */
    if (Read < On.Length)
        memset(&On.Buffer[Read], 0, On.Length - Read);

    On.Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_DATA;
    SetLine(On, true);
}

void IdeControllerDevice::FinishWrite(Channel &On)
{
    Drive *What = Selected(On);
    LARGE_INTEGER Where;
    DWORD Written = 0;

    if (What == nullptr)
    {
        Fail(On, ERROR_ABORTED);
        return;
    }

    Where.QuadPart = (LONGLONG)(On.Next * IDE_SECTOR_SIZE);
    SetFilePointerEx(What->Image, Where, nullptr, FILE_BEGIN);

    if (!WriteFile(What->Image, On.Buffer, On.Length, &Written, nullptr))
    {
        Fail(On, ERROR_UNREADABLE);
        return;
    }

    Ready(On);
}

/* WHOLE COMMANDS *************************************************************/

void IdeControllerDevice::OfferPacketData(Channel &On, ULONG Length)
{
    On.PacketTotal = Length;
    On.Offset = 0;
    On.Writing = false;

    OfferPacketBlock(On);
}

/*
 * As much of an answer as the caller said it would take at once.
 *
 * A drive told in whole commands is told beforehand how long a run it may hand
 * back in one go, and an answer longer than that comes back in several. Each
 * one is announced the same way and the next carries on where the last
 * stopped, which is why what is left is counted in the buffer rather than
 * moved down it.
 */
void IdeControllerDevice::OfferPacketBlock(Channel &On)
{
    /* A run is always a whole number of words, so an odd limit is one less */
    ULONG Limit = On.PacketLimit & ~1u;

    if (Limit == 0)
        Limit = REASON_ANY_AMOUNT;

    ULONG End = On.Offset + Limit;

    if (End > On.PacketTotal)
        End = On.PacketTotal;

    const ULONG Carrying = End - On.Offset;

    On.Length = End;
    On.Writing = false;

    /*
     * How much is waiting goes in the two registers that carry the middle of a
     * place on a drive that has one. The two were never wanted at once, and
     * the wire only ever had so many lines.
     */
    On.LbaMid = (UCHAR)(Carrying & 0xFF);
    On.LbaHigh = (UCHAR)((Carrying >> 8) & 0xFF);

    /* Bytes, and they are coming this way */
    On.Count = REASON_TO_HOST;

    On.Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_DATA;
    On.Error = 0;

    SetLine(On, true);
}

void IdeControllerDevice::PacketDone(Channel &On)
{
    On.Length = 0;
    On.Offset = 0;
    On.PacketTotal = 0;

    /* Nothing more either way, which is what both of them being set says */
    On.Count = REASON_COMMAND | REASON_TO_HOST;

    On.Status = STATUS_READY | STATUS_SEEK_DONE;
    On.Error = 0;

    SetLine(On, true);
}

void IdeControllerDevice::RunPacket(Channel &On, Drive &What)
{
    const UCHAR Which = On.Command[0];

    switch (Which)
    {
        case PACKET_TEST_UNIT_READY:
            PacketDone(On);
            break;

        case PACKET_REQUEST_SENSE:
            /* Nothing is wrong, which is what an empty answer says */
            memset(On.Buffer, 0, 18);
            On.Buffer[0] = 0x70;
            On.Buffer[7] = 10;
            OfferPacketData(On, 18);
            break;

        case PACKET_INQUIRY:
        {
            ULONG Wanted = On.Command[4];

            memset(On.Buffer, 0, 36);

            /* A drive that reads one kind of medium and is never written to */
            On.Buffer[0] = 0x05;
            On.Buffer[1] = 0x80;
            On.Buffer[3] = 0x21;
            On.Buffer[4] = 31;

            memcpy(&On.Buffer[8], "ReacTVmm", 8);
            memcpy(&On.Buffer[16], "Optical Drive   ", 16);
            memcpy(&On.Buffer[32], "0001", 4);

            if ((Wanted == 0) || (Wanted > 36))
                Wanted = 36;

            OfferPacketData(On, Wanted);
            break;
        }

        case PACKET_READ_CAPACITY:
        {
            const ULONG Last = (ULONG)(What.SectorCount - 1);

            /* Both of these are the other way round from everything else here */
            On.Buffer[0] = (UCHAR)(Last >> 24);
            On.Buffer[1] = (UCHAR)(Last >> 16);
            On.Buffer[2] = (UCHAR)(Last >> 8);
            On.Buffer[3] = (UCHAR)Last;
            On.Buffer[4] = (UCHAR)(IDE_MEDIUM_SECTOR_SIZE >> 24);
            On.Buffer[5] = (UCHAR)(IDE_MEDIUM_SECTOR_SIZE >> 16);
            On.Buffer[6] = (UCHAR)(IDE_MEDIUM_SECTOR_SIZE >> 8);
            On.Buffer[7] = (UCHAR)IDE_MEDIUM_SECTOR_SIZE;

            OfferPacketData(On, 8);
            break;
        }

        case PACKET_READ_10:
        {
            const ULONG64 First = ((ULONG64)On.Command[2] << 24) |
                                  ((ULONG64)On.Command[3] << 16) |
                                  ((ULONG64)On.Command[4] << 8) |
                                  (ULONG64)On.Command[5];
            ULONG Count = ((ULONG)On.Command[7] << 8) | On.Command[8];
            LARGE_INTEGER Where;
            DWORD Read = 0;

            if (Count == 0)
            {
                PacketDone(On);
                break;
            }

            if ((Count * IDE_MEDIUM_SECTOR_SIZE) > sizeof(On.Buffer))
                Count = sizeof(On.Buffer) / IDE_MEDIUM_SECTOR_SIZE;

            if ((First + Count) > What.SectorCount)
            {
                Fail(On, ERROR_NO_SUCH_SECTOR);
                break;
            }

            Where.QuadPart = (LONGLONG)(First * IDE_MEDIUM_SECTOR_SIZE);
            SetFilePointerEx(What.Image, Where, nullptr, FILE_BEGIN);

            if (!ReadFile(What.Image, On.Buffer,
                          Count * IDE_MEDIUM_SECTOR_SIZE, &Read, nullptr) ||
                (Read == 0))
            {
                Fail(On, ERROR_UNREADABLE);
                break;
            }

            OfferPacketData(On, Read);
            break;
        }

        default:
            Fail(On, ERROR_ABORTED);
            break;
    }
}

/* THE COMMANDS ***************************************************************/

void IdeControllerDevice::RunCommand(Channel &On, UCHAR What)
{
    Drive *Which = Selected(On);

    if (Which == nullptr)
    {
        Fail(On, ERROR_ABORTED);
        return;
    }

    switch (What)
    {
        case COMMAND_IDENTIFY:
            /*
             * A drive told in whole commands refuses this and says what it
             * really is in the two registers that carry a place. That is how
             * anything finds one without having to ask twice.
             */
            if (Which->Packet)
            {
                On.LbaMid = 0x14;
                On.LbaHigh = 0xEB;
                Fail(On, ERROR_ABORTED);
                break;
            }

            Identify(On, *Which);
            break;

        case COMMAND_IDENTIFY_PACKET:
            if (!Which->Packet)
            {
                Fail(On, ERROR_ABORTED);
                break;
            }

            Identify(On, *Which);
            break;

        case COMMAND_PACKET:
            if (!Which->Packet)
            {
                Fail(On, ERROR_ABORTED);
                break;
            }

            /*
             * Nothing happens yet. What to do arrives as twelve bytes through
             * the data register, and the drive waits for all of them.
             *
             * How much of the answer the caller will take at once was written
             * into the two registers that carry a place before this arrived,
             * and is the one thing here worth keeping out of them.
             */
            On.PacketLimit = (ULONG)On.LbaMid | ((ULONG)On.LbaHigh << 8);
            On.PacketTotal = 0;

            On.Expecting = true;
            On.CommandLength = 0;
            On.Length = 0;
            On.Offset = 0;
            On.Error = 0;

            /* What is wanted next is the command, and it comes the other way */
            On.Count = REASON_COMMAND;

            On.Status = STATUS_READY | STATUS_DATA;
            break;

        case COMMAND_READ_SECTORS:
        case COMMAND_READ_NO_RETRY:
        case COMMAND_READ_MULTIPLE:
            Begin(On, false);
            break;

        case COMMAND_WRITE_SECTORS:
        case COMMAND_WRITE_NO_RETRY:
        case COMMAND_WRITE_MULTIPLE:
            Begin(On, true);
            break;

        case COMMAND_RECALIBRATE:
        case COMMAND_SET_PARAMETERS:
        case COMMAND_SET_MULTIPLE:
        case COMMAND_FLUSH:
            Ready(On);
            break;

        default:
            Fail(On, ERROR_ABORTED);
            break;
    }
}

/* THE REGISTERS **************************************************************/

STDMETHODIMP IdeControllerDevice::NotifyIoPortRead(USHORT Port, ULONG Width,
                                                   ULONG *Value)
{
    ULONG Register = 0;
    bool IsControl = false;

    if (Value == nullptr)
        return E_POINTER;

    *Value = 0xFF;

    EnterCriticalSection(&m_Lock);

    ULONG Which = 0;
    ULONG Mover = 0;

    if (Moving(Port, Which, Mover))
    {
        ULONG Taken = 0;

        for (ULONG Index = 0; Index < Width; Index++)
        {
            const ULONG At = Mover + Index;

            if (At < IDE_MOVER_CHANNEL)
                Taken |= (ULONG)MoverRead(Which, At) << (Index * 8);
        }

        *Value = Taken;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    Channel *Where = Find(Port, &Register, &IsControl);

    if (Where == nullptr)
    {
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    Channel &On = *Where;

    if (IsControl)
    {
        /* The same as the status, without saying the interrupt was taken */
        *Value = On.Status;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    /*
     * A channel with nothing on the drive that is selected reads as empty.
     * Answering anything else is what makes something believe in a drive that
     * is not there and then wait for it.
     */
    if ((Selected(On) == nullptr) && (Register != IDE_SELECT))
    {
        *Value = 0;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    switch (Register)
    {
        case IDE_DATA:
        {
            const Drive *What = Selected(On);
            ULONG Taken = 0;

            for (ULONG Index = 0; Index < Width; Index++)
            {
                UCHAR Byte = 0;

                if (On.Offset < On.Length)
                    Byte = On.Buffer[On.Offset++];

                Taken |= (ULONG)Byte << (Index * 8);
            }

            if (On.Offset >= On.Length)
            {
                /*
                 * A drive told in whole commands says at every stop what it
                 * wants next, and says so again here: another run of the
                 * answer, or that there is none and the command is over.
                 * Anything driving one waits to be told either way.
                 */
                if ((What != nullptr) && What->Packet)
                {
                    if (On.Offset < On.PacketTotal)
                        OfferPacketBlock(On);
                    else
                        PacketDone(On);
                }
                else
                {
                    On.Status = STATUS_READY | STATUS_SEEK_DONE;
                }
            }

            *Value = Taken;
            break;
        }

        case IDE_ERROR:     *Value = On.Error; break;
        case IDE_COUNT:     *Value = On.Count; break;
        case IDE_LBA_LOW:   *Value = On.LbaLow; break;
        case IDE_LBA_MID:   *Value = On.LbaMid; break;
        case IDE_LBA_HIGH:  *Value = On.LbaHigh; break;
        case IDE_SELECT:    *Value = On.Select | 0xA0; break;

        case IDE_STATUS:
            *Value = On.Status;

            /* Reading this is what says the interrupt has been noticed */
            SetLine(On, false);
            break;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

STDMETHODIMP IdeControllerDevice::NotifyIoPortWrite(USHORT Port, ULONG Width,
                                                    ULONG Value)
{
    ULONG Register = 0;
    bool IsControl = false;

    EnterCriticalSection(&m_Lock);

    ULONG Which = 0;
    ULONG Mover = 0;

    if (Moving(Port, Which, Mover))
    {
        for (ULONG Index = 0; Index < Width; Index++)
        {
            const ULONG At = Mover + Index;

            if (At < IDE_MOVER_CHANNEL)
                MoverWrite(Which, At, (UCHAR)((Value >> (Index * 8)) & 0xFF));
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    Channel *Where = Find(Port, &Register, &IsControl);

    if (Where == nullptr)
    {
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    Channel &On = *Where;

    if (IsControl)
    {
        const UCHAR Was = On.Device;

        On.Device = (UCHAR)Value;

        if (((Was & DEVICE_RESET) == 0) && ((On.Device & DEVICE_RESET) != 0))
        {
            On.Status = STATUS_READY | STATUS_SEEK_DONE;
            On.Error = 0;
            On.Length = 0;
            On.Offset = 0;
            On.Expecting = false;
        }

        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    /* Which drive is meant is the one thing both of them always listen for */
    if (Register == IDE_SELECT)
    {
        On.Select = (UCHAR)Value;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    if (Selected(On) == nullptr)
    {
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    switch (Register)
    {
        case IDE_DATA:
            /* A drive waiting to be told what to do is being told, not written to */
            if (On.Expecting)
            {
                for (ULONG Index = 0; Index < Width; Index++)
                {
                    if (On.CommandLength < IDE_PACKET_LENGTH)
                    {
                        On.Command[On.CommandLength++] =
                            (UCHAR)((Value >> (Index * 8)) & 0xFF);
                    }
                }

                if (On.CommandLength >= IDE_PACKET_LENGTH)
                {
                    On.Expecting = false;
                    RunPacket(On, *Selected(On));
                }

                break;
            }

            /*
             * A drive that is not waiting to be written to takes nothing. What
             * arrives here otherwise is the tail of a caller that wrote more
             * of a whole command than the command is long, and letting it
             * through would put it over the answer that was just prepared.
             */
            if (!On.Writing)
                break;

            for (ULONG Index = 0; Index < Width; Index++)
            {
                if (On.Offset < sizeof(On.Buffer))
                    On.Buffer[On.Offset++] = (UCHAR)((Value >> (Index * 8)) & 0xFF);
            }

            if (On.Offset >= On.Length)
                FinishWrite(On);

            break;

        case IDE_FEATURES:  On.Features = (UCHAR)Value; break;
        case IDE_COUNT:     On.Count = (UCHAR)Value; break;
        case IDE_LBA_LOW:   On.LbaLow = (UCHAR)Value; break;
        case IDE_LBA_MID:   On.LbaMid = (UCHAR)Value; break;
        case IDE_LBA_HIGH:  On.LbaHigh = (UCHAR)Value; break;

        case IDE_COMMAND:
            RunCommand(On, (UCHAR)Value);
            break;
    }

    LeaveCriticalSection(&m_Lock);
    return S_OK;
}

/* WHAT IS IN THE DRIVES ******************************************************/

void IdeControllerDevice::Shape(Drive &What)
{
    ULONG64 Cylinders;

    What.Heads = SHAPE_HEADS;
    What.Sectors = SHAPE_SECTORS;

    Cylinders = What.SectorCount / (What.Heads * What.Sectors);

    if (Cylinders == 0)
        Cylinders = 1;

    if (Cylinders > 65535)
        Cylinders = 65535;

    What.Cylinders = (ULONG)Cylinders;
}

bool IdeControllerDevice::Attach(Drive &What, const char *Path, bool Optical,
                                 bool ReadOnly)
{
    LARGE_INTEGER Size;

    strncpy(What.Path, Path, sizeof(What.Path) - 1);
    What.Path[sizeof(What.Path) - 1] = '\0';

    What.Packet = Optical;
    What.ReadOnly = ReadOnly || Optical;

    What.Image = CreateFileA(What.Path,
                             What.ReadOnly
                                 ? GENERIC_READ
                                 : (GENERIC_READ | GENERIC_WRITE),
                             FILE_SHARE_READ,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);

    /* One that will not open for writing is still worth having read only */
    if ((What.Image == INVALID_HANDLE_VALUE) && !What.ReadOnly)
    {
        What.Image = CreateFileA(What.Path, GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);

        if (What.Image != INVALID_HANDLE_VALUE)
            What.ReadOnly = true;
    }

    if (What.Image == INVALID_HANDLE_VALUE)
        return false;

    if (!GetFileSizeEx(What.Image, &Size))
    {
        CloseHandle(What.Image);
        What.Image = INVALID_HANDLE_VALUE;
        return false;
    }

    What.SectorCount = (ULONG64)Size.QuadPart /
                       (Optical ? IDE_MEDIUM_SECTOR_SIZE : IDE_SECTOR_SIZE);

    if (!Optical)
        Shape(What);

    What.Present = true;
    return true;
}

/**
 * @brief
 * Takes what the machine was told to put in the drives.
 *
 * @remarks
 * Four of them, named for where they hang rather than for what they are, which
 * is how a cable has always been described. A name followed by a path attaches
 * something; a path followed by a comma and the word for an optical drive makes
 * it one of those.
 */
STDMETHODIMP IdeControllerDevice::SetSettings(const char *Settings)
{
    static const char *const Names[] =
    {
        "primary-master", "primary-slave",
        "secondary-master", "secondary-slave"
    };

    if (Settings == nullptr)
        return S_OK;

    for (ULONG Index = 0; Index < ARRAYSIZE(Names); Index++)
    {
        const char *Found = strstr(Settings, Names[Index]);

        if ((Found == nullptr) || (Found[strlen(Names[Index])] != '='))
            continue;

        const char *Value = Found + strlen(Names[Index]) + 1;
        char Path[MAX_PATH];
        ULONG Length = 0;

        while ((Length + 1 < sizeof(Path)) && (Value[Length] != '\0') &&
               (Value[Length] != ',') && (Value[Length] != ';'))
        {
            Path[Length] = Value[Length];
            Length++;
        }

        Path[Length] = '\0';

        const bool Optical = (Value[Length] == ',') &&
                             (strncmp(&Value[Length + 1], "cdrom", 5) == 0);

        Drive &What = m_Channel[Index / 2].Drives[Index % 2];

        if (!Attach(What, Path, Optical, false))
            return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

} /* namespace rtvm */
