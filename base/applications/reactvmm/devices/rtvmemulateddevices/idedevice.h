/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The controller the disks and the optical drive hang off
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "vdevbase.h"

#include <string.h>

namespace rtvm
{

/* Where the two channels answer, and the register each keeps apart */
#define IDE_PRIMARY_BASE        0x01F0
#define IDE_PRIMARY_CONTROL     0x03F6
#define IDE_PRIMARY_LINE        14
#define IDE_SECONDARY_BASE      0x0170
#define IDE_SECONDARY_CONTROL   0x0376
#define IDE_SECONDARY_LINE      15

#define IDE_REGISTER_COUNT      8

/* What a drive of each kind carries at a time */
#define IDE_SECTOR_SIZE         512
#define IDE_MEDIUM_SECTOR_SIZE  2048

/* As much as one command may move, which is a whole run of sectors */
#define IDE_BUFFER_SIZE         (IDE_MEDIUM_SECTOR_SIZE * 64)

/* How long a whole command is, for a drive that is told in those */
#define IDE_PACKET_LENGTH       12

/* Where on the bus this controller is put, and what it says it is there */
#define IDE_BUS_DEVICE          7
#define IDE_BUS_FUNCTION        1
#define IDE_WHO_IT_IS           0x71118086
#define IDE_WHAT_IT_IS          0x01018001

/* Where in its description each of the things it answers about is */
#define IDE_PCI_WHO             0x00
#define IDE_PCI_DOING           0x04
#define IDE_PCI_WHAT            0x08
#define IDE_PCI_MOVER           0x20
#define IDE_PCI_TIMING          0x40

/* The run of ports the part that would move data on its own answers on */
#define IDE_MOVER_LENGTH        16
#define IDE_MOVER_CHANNEL       8

/* Which of those does what, from the base of a channel's half */
#define IDE_MOVER_COMMAND       0
#define IDE_MOVER_STATUS        2
#define IDE_MOVER_LIST          4

/* That what it answers on is a run of ports, which is not its to change */
#define IDE_MOVER_IS_PORTS      0x01

/* And what the two of them carry */
#define IDE_MOVER_STARTED       0x01
#define IDE_MOVER_DIRECTION     0x08
#define IDE_MOVER_RUNNING       0x01
#define IDE_MOVER_FAILED        0x02
#define IDE_MOVER_FINISHED      0x04

/* Which of the things it could be doing this machine has any meaning for */
#define IDE_DECODES_PORTS       0x0001
#define IDE_MOVES_ITS_OWN       0x0004

/* The bits of the description that are put back whatever is written */
#define IDE_DOING_KEPT          (IDE_DECODES_PORTS | IDE_MOVES_ITS_OWN)
#define IDE_DOING_CLEARED       0x38000000
#define IDE_DOING_AT_REST       0x02800000

class IdeControllerDevice : public VirtualDeviceBase,
                            public IVndIoPortHandler,
                            public IVmPciConfigAccessHandler,
                            public IRtvmDeviceSettings
{
public:
    IdeControllerDevice();
    ~IdeControllerDevice() override;

    IdeControllerDevice(const IdeControllerDevice &) = delete;
    IdeControllerDevice &operator=(const IdeControllerDevice &) = delete;

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return VirtualDeviceBase::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return VirtualDeviceBase::Release(); }

    STDMETHODIMP GetDependencies(void *Repository, ULONG *Count,
                                 GUID **Services, ULONG *Required) override;
    STDMETHODIMP StartReservingResources(void *Repository, VDEV_STATE State) override;
    STDMETHODIMP PowerOnCold(VDEV_STATE State) override;
    STDMETHODIMP PowerOff(VDEV_STATE State) override;
    STDMETHODIMP Reset(VDEV_STATE State) override;

    /* What the machine was told to put in the drives */
    STDMETHODIMP SetSettings(const char *Settings) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, USHORT Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, USHORT Width, ULONG Value) override;

    /* What it says it is to whatever is walking the bus */
    STDMETHODIMP NotifyPciConfigAccess(UCHAR Bus, UCHAR Device, UCHAR Function,
                                       USHORT Offset, UCHAR Writing,
                                       ULONG *Value) override;

private:
    struct Drive
    {
        HANDLE Image;
        ULONG64 SectorCount;
        bool Present;
        bool ReadOnly;

        /* Whether it is told in whole commands rather than register writes */
        bool Packet;

        /* The shape it is described as having, which a packet drive has none of */
        ULONG Cylinders;
        ULONG Heads;
        ULONG Sectors;

        CHAR Path[MAX_PATH];
    };

    struct Channel
    {
        USHORT Base;
        USHORT Control;
        ULONG Line;

        Drive Drives[2];

        /* The registers, as last written */
        UCHAR Features;
        UCHAR Count;
        UCHAR LbaLow;
        UCHAR LbaMid;
        UCHAR LbaHigh;
        UCHAR Select;
        UCHAR Status;
        UCHAR Error;
        UCHAR Device;

        /* Where whatever is being carried in or out is held */
        UCHAR Buffer[IDE_BUFFER_SIZE];
        ULONG Length;
        ULONG Offset;
        bool Writing;

        /* A drive told in whole commands, being told one */
        bool Expecting;
        UCHAR Command[IDE_PACKET_LENGTH];
        ULONG CommandLength;

        /* How much is still to come after what is being carried */
        ULONG Remaining;
        ULONG64 Next;

        /*
         * The whole of what a drive told in whole commands has to hand back,
         * and the most of it the caller said it would take at once. An answer
         * longer than that comes back in several goes.
         */
        ULONG PacketTotal;
        ULONG PacketLimit;

        bool LineAsserted;
    };

    Channel *Find(USHORT Port, ULONG *Register, bool *IsControl);
    Drive *Selected(Channel &On);

    void SetLine(Channel &On, bool Asserted);
    void Fail(Channel &On, UCHAR Why);
    void Ready(Channel &On);

    void Identify(Channel &On, Drive &What);
    void Begin(Channel &On, bool Writing);
    void FinishWrite(Channel &On);
    ULONG64 Place(Channel &On, Drive &What) const;

    void RunCommand(Channel &On, UCHAR What);
    void RunPacket(Channel &On, Drive &What);
    void OfferPacketData(Channel &On, ULONG Length);
    void OfferPacketBlock(Channel &On);
    void PacketDone(Channel &On);

    bool Attach(Drive &What, const char *Path, bool Optical, bool ReadOnly);
    static void Shape(Drive &What);

    /* The ports of the part that would move data, once the guest sites them */
    bool Moving(USHORT Port, ULONG &Which, ULONG &Register) const;
    void PlaceMover();
    UCHAR MoverRead(ULONG Which, ULONG Register) const;
    void MoverWrite(ULONG Which, ULONG Register, UCHAR Byte);

    CRITICAL_SECTION m_Lock = {};
    Channel m_Channel[2] = {};
    IVmIoApic *m_Lines = nullptr;

    /*
     * What the guest has written into the description of this controller. Only
     * the two bits of the first that this machine has any meaning for are kept,
     * and the rest is remembered so that a write reads back.
     */
    ULONG m_Doing = 0;
    ULONG m_MoverPorts = 0;
    ULONG m_Timing[3] = {};

    /*
     * Where the part that would move data on its own was sited, and what its
     * two halves last answered. Nothing ever starts it: no drive here says it
     * can be moved from that way, so the guest never asks.
     */
    bool m_MoverPlaced = false;
    UCHAR m_MoverCommand[2] = {};
    UCHAR m_MoverStatus[2] = {};
    ULONG m_MoverList[2] = {};
};

} /* namespace rtvm */
