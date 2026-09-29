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

class IdeControllerDevice : public VirtualDeviceBase,
                            public IVndIoPortHandler,
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
                                 GUID **Services, ULONG *Optional) override;
    STDMETHODIMP StartReservingResources() override;
    STDMETHODIMP PowerOnCold() override;
    STDMETHODIMP PowerOff() override;
    STDMETHODIMP Reset() override;

    /* What the machine was told to put in the drives */
    STDMETHODIMP SetSettings(const char *Settings) override;

    STDMETHODIMP NotifyUnregistered() override { return S_OK; }
    STDMETHODIMP NotifyIoPortRead(USHORT Port, ULONG Width, ULONG *Value) override;
    STDMETHODIMP NotifyIoPortWrite(USHORT Port, ULONG Width, ULONG Value) override;

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
    void PacketDone(Channel &On);

    bool Attach(Drive &What, const char *Path, bool Optical, bool ReadOnly);
    static void Shape(Drive &What);

    CRITICAL_SECTION m_Lock = {};
    Channel m_Channel[2] = {};
    IVmIoApic *m_Lines = nullptr;
};

} /* namespace rtvm */
