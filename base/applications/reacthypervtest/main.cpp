/*
 * PROJECT:     ReactHypervTest
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driving the hardware a real machine of this kind is built from
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Everything in ReacTVmm is written against a contract that was read out of
 * somebody else's binaries: the identifiers a device is asked for by, the
 * order of the calls it is driven through, and the services it is handed to
 * reach the rest of the machine. All of that was worked out by reading, and
 * reading is not the same as being right.
 *
 * This is how it is checked. It loads the real hardware rather than ours,
 * asks it for a device, and drives it through the same contract. A device
 * that comes up and asks for what it needs says the contract is right. One
 * that refuses says which part of it is not, and that is worth more than
 * another afternoon of reading.
 */

/* One place where the identifiers are the things themselves, not names for them */
#define INITGUID

#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vdev.h"

/* WHAT A GUID IS CALLED ******************************************************/

/*
 * The ones worked out so far, so that what a device asks for comes back as a
 * name rather than as a number nobody can hold in their head. Anything not
 * here is printed as itself, which is how something nobody has seen before
 * shows up as worth looking into.
 */
static const struct
{
    const GUID *Which;
    const char *Name;
} KnownNames[] =
{
    { &IID_IUnknown, "IUnknown" },
    { &IID_IVirtualDevice, "IVirtualDevice" },
    { &IID_IVmServiceAccess, "IVmServiceAccess" },
    { &IID_IVmAmd64EmulationServices, "IVmAmd64EmulationServices" },
    { &IID_IVmProcessorServices, "IVmProcessorServices" },
    { &IID_IVmGuestMemoryAccess, "IVmGuestMemoryAccess" },
    { &IID_IVmIoApic, "IVmIoApic" },
    { &IID_IVmPicService, "IVmPicService" },
    { &IID_IVmPitService, "IVmPitService" },
    { &IID_IVmDmaController, "IVmDmaController" },
    { &IID_IVmPciBusService, "IVmPciBusService" },
    { &IID_IVmPciConfigAccessHandler, "IVmPciConfigAccessHandler" },
    { &IID_IVmInstalledPciDevice, "IVmInstalledPciDevice" },
    { &IID_IVmSuperIo, "IVmSuperIo" },
    { &IID_IVmInputController, "IVmInputController" },
    { &IID_IVmBios, "IVmBios" },
    { &IID_IVmTimeSource, "IVmTimeSource" },
    { &IID_IVmPowerServices, "IVmPowerServices" },
    { &IID_IVmBootMemoryTopology, "IVmBootMemoryTopology" },
    { &IID_IVmMemoryTopology, "IVmMemoryTopology" },
    { &IID_IVmMemoryManagement, "IVmMemoryManagement" },
    { &IID_IVmbusServices, "IVmbusServices" },
    { &IID_IVpciServices, "IVpciServices" },
    { &IID_IVmPartitionServices, "IVmPartitionServices" },
    { &IID_ISecurityManager, "ISecurityManager" },
    { &IID_IVmManagementAccess, "IVmManagementAccess" },
    { &IID_IVmHandleBrokerServices, "IVmHandleBrokerServices" },
    { &IID_IVmBootStateImporter, "IVmBootStateImporter" },
    { &IID_IVmGuestStateAccess, "IVmGuestStateAccess" },
    { &IID_IVmGuestStateRawStorage, "IVmGuestStateRawStorage" },
    { &IID_IVmGuestCrashServices, "IVmGuestCrashServices" },
    { &IID_IVmCrashRegisterServices, "IVmCrashRegisterServices" },
    { &IID_IVmPowerManagementDevice, "IVmPowerManagementDevice" },
    { &IID_IVmBattery, "IVmBattery" },
    { &IID_IVpmemController, "IVpmemController" },
    { &IID_IVmPsp, "IVmPsp" },
    { &IID_IProxiedPciVgaDevice, "IProxiedPciVgaDevice" },
    { &IID_IMonitorDevice, "IMonitorDevice" },
    { &IID_IVideoVdev, "IVideoVdev" },
    { &IID_IVndIoPortHandler, "IVndIoPortHandler" },
    { &IID_IVndMmioHandler, "IVndMmioHandler" },
    { &IID_IVmTimerHandler, "IVmTimerHandler" }
};

static void SayGuid(const GUID &Which)
{
    for (const auto &Known : KnownNames)
    {
        if (IsEqualGUID(Which, *Known.Which))
        {
            printf("%s", Known.Name);
            return;
        }
    }

    printf("{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
           Which.Data1, Which.Data2, Which.Data3,
           Which.Data4[0], Which.Data4[1], Which.Data4[2], Which.Data4[3],
           Which.Data4[4], Which.Data4[5], Which.Data4[6], Which.Data4[7]);
}

/*
 * What a device is handed for a range it reserved.
 *
 * It has to be its own object. A device that is switched off revokes through
 * this and then lets go of it, so handing back the device's own handler makes
 * that second call a Release on the device itself and frees it underneath the
 * manager still driving it.
 */
class Reservation : public IVndRegistration
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown))
        {
            *Object = static_cast<IVndRegistration *>(this);
            AddRef();
            return S_OK;
        }

        *Object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG Left = (ULONG)InterlockedDecrement(&m_Count);

        if (Left == 0)
            delete this;

        return Left;
    }

    STDMETHODIMP Revoke() override
    {
        printf("    gives a range back\n");
        return S_OK;
    }

private:
    ~Reservation() = default;

    volatile LONG m_Count = 1;
};

/*
 * Where a device asks for the addresses it answers for.
 *
 * Nothing is reserved. What matters is which of these is called and what it is
 * handed: a device asking for the two ports the interrupt controller has
 * always had, in that order, says the slots in this were read right. One
 * asking for something senseless says they were not, and which call the
 * nonsense arrived at says where the reading went wrong.
 */
class Emulation : public IVmAmd64EmulationServices
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IVmAmd64EmulationServices))
        {
            *Object = static_cast<IVmAmd64EmulationServices *>(this);
            AddRef();
            return S_OK;
        }

        *Object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        return (ULONG)InterlockedDecrement(&m_Count);
    }

    STDMETHODIMP RegisterMmioHandler(ULONG64 FirstPage, ULONG64 PageCount,
                                     IVndMmioHandler *Handler, BOOL Enabled,
                                     IVndRegistration **Registration) override
    {
        printf("    memory: %llu page(s) at %012llx, handler %p, %s\n",
               (unsigned long long)PageCount,
               (unsigned long long)(FirstPage * VDEV_PAGE_SIZE),
               (void *)Handler, Enabled ? "on" : "off");

        if (Registration != nullptr)
            *Registration = new Reservation();

        return S_OK;
    }

    STDMETHODIMP RegisterMbHandler() override
    {
        printf("    the mailbox\n");
        return E_NOTIMPL;
    }

    STDMETHODIMP RegisterApicEoiHandler() override
    {
        printf("    the cycle that says an interrupt is finished\n");
        return E_NOTIMPL;
    }

    STDMETHODIMP RegisterIoPortHandler(USHORT FirstPort, USHORT LastPort,
                                       ULONG Widths, IVndIoPortHandler *Handler,
                                       ULONG Flags,
                                       IVndRegistration **Registration) override
    {
        printf("    ports: %04x to %04x, widths %lx, handler %p, flags %lx\n",
               FirstPort, LastPort, Widths, (void *)Handler, Flags);

        if (m_Ports < (ULONG)(sizeof(m_Port) / sizeof(*m_Port)))
        {
            m_Port[m_Ports].First = FirstPort;
            m_Port[m_Ports].Last = LastPort;
            m_Port[m_Ports].Widths = Widths;
            m_Port[m_Ports].Handler = Handler;
        }

        m_Ports++;

        if (Registration != nullptr)
            *Registration = new Reservation();

        return S_OK;
    }

    STDMETHODIMP RegisterMsrHandler() override
    {
        printf("    a machine specific register\n");
        return E_NOTIMPL;
    }

    STDMETHODIMP RegisterExceptionHandler() override
    {
        printf("    a fault\n");
        return E_NOTIMPL;
    }

    /* A run of ports somebody asked to answer for, kept to be driven later */
    struct Reserved
    {
        USHORT First;
        USHORT Last;
        ULONG Widths;
        IVndIoPortHandler *Handler;
    };

    /* What was asked for, and what there was room to write down */
    ULONG Ports() const noexcept { return m_Ports; }
    ULONG Kept() const noexcept
    {
        const ULONG Room = (ULONG)(sizeof(m_Port) / sizeof(*m_Port));

        return (m_Ports < Room) ? m_Ports : Room;
    }

    const Reserved &Port(ULONG Which) const noexcept { return m_Port[Which]; }

    /* Whoever answers for a port, or nothing if it was never asked for */
    IVndIoPortHandler *Claimed(USHORT Port) const noexcept
    {
        for (ULONG Index = 0; Index < Kept(); Index++)
        {
            if ((Port >= m_Port[Index].First) && (Port <= m_Port[Index].Last))
                return m_Port[Index].Handler;
        }

        return nullptr;
    }

private:
    volatile LONG m_Count = 1;

    Reserved m_Port[32] = {};
    ULONG m_Ports = 0;
};

/*
 * Where a device raises its line.
 *
 * Nothing is delivered. A device that raises the line its kind has always had,
 * and names itself as the one raising it, says the slots here were read right
 * in the same way the ports do: the line number is the answer.
 */
class Lines : public IVmIoApic
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IVmIoApic))
        {
            *Object = static_cast<IVmIoApic *>(this);
            AddRef();
            return S_OK;
        }

        *Object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        return (ULONG)InterlockedDecrement(&m_Count);
    }

    STDMETHODIMP WaitForIrqAssert(ULONG Line) override
    {
        UNREFERENCED_PARAMETER(Line);
        return E_NOTIMPL;
    }

    STDMETHODIMP AssertIrq(UCHAR Line, UCHAR Source) override
    {
        printf("    line %u up, raised by %u\n", Line, Source);
        m_Raised++;
        return S_OK;
    }

    STDMETHODIMP DeassertIrq(UCHAR Line, UCHAR Source) override
    {
        printf("    line %u down, let go by %u\n", Line, Source);
        return S_OK;
    }

    STDMETHODIMP RequestTimerAssist(UCHAR Line, ULONG64 Period,
                                    int *Assisted, int *StillWanted) override
    {
        printf("    line %u asks to be left alone for %llu\n", Line,
               (unsigned long long)Period);

        if ((Assisted == nullptr) || (StillWanted == nullptr))
            return E_POINTER;

        *Assisted = 0;
        *StillWanted = 1;
        return S_OK;
    }

    STDMETHODIMP DeclineTimerAssist(UCHAR Line) override
    {
        UNREFERENCED_PARAMETER(Line);
        return S_OK;
    }

    STDMETHODIMP RegisterRteChangeCallback(UCHAR Line,
                                           IUnknown *Callback) override
    {
        UNREFERENCED_PARAMETER(Callback);
        printf("    wants telling where line %u goes\n", Line);
        return S_OK;
    }

    STDMETHODIMP UnregisterRteChangeCallback(UCHAR Line) override
    {
        UNREFERENCED_PARAMETER(Line);
        return S_OK;
    }

    STDMETHODIMP SetIoApicBaseAddress(ULONG Address) override
    {
        printf("    moves the table to %08lx\n", Address);
        return S_OK;
    }

    ULONG Raised() const noexcept { return m_Raised; }

private:
    volatile LONG m_Count = 1;
    ULONG m_Raised = 0;
};

/*
 * The processors, as a device with something for them sees them. One processor
 * and nothing to offer it: what is being checked here is whether a device that
 * is handed this is satisfied by it, not what it does with it afterwards.
 */
class Processors : public IVmProcessorServices
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IVmProcessorServices))
        {
            *Object = static_cast<IVmProcessorServices *>(this);
            AddRef();
            return S_OK;
        }

        *Object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        return (ULONG)InterlockedDecrement(&m_Count);
    }

    STDMETHODIMP GetVirtualProcessorCount(ULONG *Count) override
    {
        printf("    how many processors\n");

        if (Count != nullptr)
            *Count = 1;

        return S_OK;
    }

    STDMETHODIMP SetVirtualProcessorState() override { return E_NOTIMPL; }
    STDMETHODIMP GetVirtualProcessorState() override { return E_NOTIMPL; }

    STDMETHODIMP AssertVirtualProcessorInterrupt(ULONG64 Delivery,
                                                 ULONG64 Reserved,
                                                 ULONG Vector) override
    {
        printf("    a vector is owed: %lx, delivery %llx, spare %llx\n",
               Vector, (unsigned long long)Delivery,
               (unsigned long long)Reserved);
        return S_OK;
    }

    STDMETHODIMP ClearVirtualProcessorInterrupt() override { return S_OK; }
    STDMETHODIMP ConfigureInterceptThrottlingExclusion() override { return E_NOTIMPL; }
    STDMETHODIMP StopAllVirtualProcessors() override { return E_NOTIMPL; }
    STDMETHODIMP StartAllVirtualProcessors() override { return E_NOTIMPL; }

private:
    volatile LONG m_Count = 1;
};

/* WHAT A DEVICE READS ITSELF OUT OF ******************************************/

/*
 * The store a device is told what it is by.
 *
 * Nothing is known about the shape of this one: it is not named in anything
 * that has been read, and a device reaches into it before it does anything
 * else. So it is built the other way round, as a run of slots that each say
 * they were called and hand back nothing. What a device asks for, and in what
 * order, comes out of running it.
 *
 * Laid out by hand rather than declared, because a table whose shape is the
 * thing being discovered cannot be written as a class.
 */
#define REPOSITORY_SLOTS 48

struct RepositoryObject
{
    const void **Vtable;
    volatile LONG Count;
};

static bool RepositoryQuiet = false;

/* What a slot nobody has worked out yet answers, which is worth trying both ways */
static HRESULT RepositoryAnswer = S_OK;

template <int Slot>
static HRESULT STDMETHODCALLTYPE RepositorySlot(void *This, void *First,
                                                void *Second, void *Third)
{
    UNREFERENCED_PARAMETER(This);

    if (!RepositoryQuiet)
    {
        printf("    store slot %d (%p %p %p)\n", Slot, First, Second, Third);

        /*
         * And whatever it was asked about, because a slot nobody has named is
         * named by what is handed to it. Guarded, since nothing here knows
         * that the thing handed over is a pointer at all.
         */
        __try
        {
            const auto *Bytes = static_cast<const UCHAR *>(Second);

            printf("      about:");

            for (ULONG Index = 0; Index < 32; Index++)
                printf(" %02x", Bytes[Index]);

            printf("\n            ");

            for (ULONG Index = 0; Index < 32; Index++)
            {
                const UCHAR One = Bytes[Index];

                printf("%c", ((One >= 0x20) && (One < 0x7F)) ? (char)One : '.');
            }

            printf("\n");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("      about: not something that can be read\n");
        }
    }

    return RepositoryAnswer;
}

/*
 * The one slot that is known: what this device is called. A device walks what
 * comes back looking for the end of it, so handing back nothing is handing
 * back a walk off the end of memory.
 */
static HRESULT STDMETHODCALLTYPE RepositoryName(void *This, BSTR *Name,
                                                const GUID *Which)
{
    UNREFERENCED_PARAMETER(This);
    UNREFERENCED_PARAMETER(Which);

    printf("    store: asked what this device is called\n");

    if (Name == nullptr)
        return E_POINTER;

    /*
     * Allocated the way the caller will free it. What comes back here is let
     * go of with the call that frees a string of that kind, so handing back
     * anything else is handing back something that will be freed as though it
     * were one, and the whole process goes down rather than the call failing.
     */
    *Name = SysAllocString(L"ReactHypervTest");

    return (*Name != nullptr) ? S_OK : E_OUTOFMEMORY;
}

/*
 * And the kind of device it is, which is a second identifier beside the one it
 * was made under. Nothing here has more than one machine's worth of devices,
 * so they are all of the same kind and it does not matter which; what matters
 * is that one is written, because the caller turns it into a string straight
 * after and a caller reading uninitialised memory is a caller doing anything.
 */
static HRESULT STDMETHODCALLTYPE RepositoryKind(void *This, void *Spare,
                                                GUID *Kind)
{
    UNREFERENCED_PARAMETER(This);

    printf("    store: asked what kind of device this is\n");

    if (Spare != nullptr)
        memset(Spare, 0, 16);

    if (Kind != nullptr)
        memset(Kind, 0, sizeof(*Kind));

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE RepositoryQuery(void *This, REFIID Interface,
                                                 void **Object)
{
    auto *Self = static_cast<RepositoryObject *>(This);

    if (Object == nullptr)
        return E_POINTER;

    printf("    store: asked for ");
    SayGuid(Interface);
    printf("\n");

    if (IsEqualIID(Interface, IID_IUnknown))
    {
        InterlockedIncrement(&Self->Count);
        *Object = This;
        return S_OK;
    }

    *Object = nullptr;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE RepositoryHold(void *This)
{
    return (ULONG)InterlockedIncrement(&static_cast<RepositoryObject *>(This)->Count);
}

static ULONG STDMETHODCALLTYPE RepositoryDrop(void *This)
{
    return (ULONG)InterlockedDecrement(&static_cast<RepositoryObject *>(This)->Count);
}

#define SLOT(n) reinterpret_cast<const void *>(&RepositorySlot<n>)

static const void *RepositoryVtable[REPOSITORY_SLOTS] =
{
    reinterpret_cast<const void *>(&RepositoryQuery),
    reinterpret_cast<const void *>(&RepositoryHold),
    reinterpret_cast<const void *>(&RepositoryDrop),
    reinterpret_cast<const void *>(&RepositoryKind),
    SLOT(4),  SLOT(5),  SLOT(6),  SLOT(7),
    SLOT(8),  SLOT(9),  SLOT(10),
    reinterpret_cast<const void *>(&RepositoryName),
    SLOT(12), SLOT(13), SLOT(14), SLOT(15),
    SLOT(16), SLOT(17), SLOT(18), SLOT(19),
    SLOT(20), SLOT(21), SLOT(22), SLOT(23),
    SLOT(24), SLOT(25), SLOT(26), SLOT(27),
    SLOT(28), SLOT(29), SLOT(30), SLOT(31),
    SLOT(32), SLOT(33), SLOT(34), SLOT(35),
    SLOT(36), SLOT(37), SLOT(38), SLOT(39),
    SLOT(40), SLOT(41), SLOT(42), SLOT(43),
    SLOT(44), SLOT(45), SLOT(46), SLOT(47)
};

static RepositoryObject TheRepository = { RepositoryVtable, 1 };

/* WHAT THE DEVICE IS HANDED **************************************************/

/*
 * Everything a device reaches the rest of the machine through, and none of it
 * answered.
 *
 * A device is given this and asks it for each of the services it named. What
 * it asks for is written down and then refused, because the point here is the
 * asking: a list of what the real hardware wants is the list of what has to be
 * built before any of it will run.
 */
class Probe : public IVmServiceAccess
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IVmServiceAccess))
        {
            *Object = static_cast<IVmServiceAccess *>(this);
            AddRef();
            return S_OK;
        }

        printf("  the device wanted ");
        SayGuid(Interface);
        printf(" out of what it was handed\n");

        *Object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        return (ULONG)InterlockedDecrement(&m_Count);
    }

    STDMETHODIMP GetService(REFIID Service, void **Object) override
    {
        if (Object != nullptr)
            *Object = nullptr;

        printf("  asks for ");
        SayGuid(Service);

        m_Asked++;

        /* The ones that are answered, being the ones worth checking */
        if (Object != nullptr)
        {
            if (IsEqualIID(Service, IID_IVmAmd64EmulationServices))
            {
                printf(" (given)\n");
                m_Emulation.AddRef();
                *Object = static_cast<IVmAmd64EmulationServices *>(&m_Emulation);
                return S_OK;
            }

            if (IsEqualIID(Service, IID_IVmProcessorServices))
            {
                printf(" (given)\n");
                m_Processors.AddRef();
                *Object = static_cast<IVmProcessorServices *>(&m_Processors);
                return S_OK;
            }

            if (IsEqualIID(Service, IID_IVmIoApic))
            {
                printf(" (given)\n");
                m_Lines.AddRef();
                *Object = static_cast<IVmIoApic *>(&m_Lines);
                return S_OK;
            }
        }

        printf("\n");
        return E_NOINTERFACE;
    }

    ULONG Asked() const noexcept { return m_Asked; }
    const Emulation &Addresses() const noexcept { return m_Emulation; }
    const Lines &Wires() const noexcept { return m_Lines; }

private:
    volatile LONG m_Count = 1;
    ULONG m_Asked = 0;
    Emulation m_Emulation;
    Processors m_Processors;
    Lines m_Lines;
};

/* DRIVING ONE ****************************************************************/

typedef HRESULT (STDAPICALLTYPE *PFN_GET_CLASS_OBJECT)(REFCLSID, REFIID,
                                                       void **);

/* What the pair of chips is set up with, and a mask picked to be unmistakable */
#define PIC_START           0x11
#define PIC_FIRST_VECTOR    0x08
#define PIC_SECOND_ON_LINE  0x04
#define PIC_LIKE_AN_8086    0x01
#define PIC_A_MASK          0xAB

/*
 * Writes what an interrupt controller is set up with and reads back the mask.
 *
 * A device that hands back the byte it was given has kept state across two
 * calls it decoded itself, through ports it asked for through a slot nobody
 * documented. Nothing short of the whole chain being right answers correctly.
 */
static void DrivePic(const Emulation &Addresses)
{
    IVndIoPortHandler *Command = Addresses.Claimed(0x20);
    IVndIoPortHandler *Data = Addresses.Claimed(0x21);

    if ((Command == nullptr) || (Data == nullptr))
    {
        printf("it never asked for 0020 and 0021, so there is nothing to "
               "drive\n");
        return;
    }

    Command->NotifyIoPortWrite(0x20, 1, PIC_START);
    Data->NotifyIoPortWrite(0x21, 1, PIC_FIRST_VECTOR);
    Data->NotifyIoPortWrite(0x21, 1, PIC_SECOND_ON_LINE);
    Data->NotifyIoPortWrite(0x21, 1, PIC_LIKE_AN_8086);
    Data->NotifyIoPortWrite(0x21, 1, PIC_A_MASK);

    ULONG Value = 0xFFFFFFFF;
    const HRESULT Status = Data->NotifyIoPortRead(0x21, 1, &Value);

    printf("set it up and read the mask back: %08lx, %02lx\n", Status, Value);

    if ((SUCCEEDED(Status)) && ((Value & 0xFF) == PIC_A_MASK))
        printf("which is what was written, so it is running\n");
    else
        printf("which is not what was written\n");
}

/*
 * Whatever can be asked of the kind that was loaded. Only the pair of chips
 * has an answer worth checking; for anything else, reading the first port it
 * asked for at least says it decodes rather than faults.
 */
static void Drive(const Probe &Handed, const GUID &Which)
{
    const Emulation &Addresses = Handed.Addresses();

    if (Addresses.Ports() == 0)
        return;

    if (IsEqualGUID(Which, CLSID_PicDevice))
    {
        DrivePic(Addresses);
        return;
    }

    for (ULONG Index = 0; Index < Addresses.Kept(); Index++)
    {
        const Emulation::Reserved &One = Addresses.Port(Index);
        ULONG Value = 0xFFFFFFFF;

        const HRESULT Status = One.Handler->NotifyIoPortRead(One.First, 1,
                                                             &Value);

        printf("reading %04x: %08lx, %02lx\n", One.First, Status,
               Value & 0xFF);
    }
}

static bool ReadGuid(const char *Text, GUID &Which)
{
    unsigned long Data1 = 0;
    unsigned int Part[11] = {};

    if (sscanf(Text, "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &Data1, &Part[0], &Part[1], &Part[2], &Part[3], &Part[4],
               &Part[5], &Part[6], &Part[7], &Part[8], &Part[9]) != 11)
    {
        return false;
    }

    Which.Data1 = (ULONG)Data1;
    Which.Data2 = (USHORT)Part[0];
    Which.Data3 = (USHORT)Part[1];

    for (ULONG Index = 0; Index < 8; Index++)
        Which.Data4[Index] = (UCHAR)Part[2 + Index];

    return true;
}

namespace hv { int Run(const char *Path, ULONG Steps, ULONG64 Ram); }

static void Usage()
{
    printf(
        "ReactHypervTest, which drives real hardware through our contract\n"
        "\n"
        "  reacthypervtest --library <file> --class <guid>\n"
        "\n"
        "  --library <file>   The class server to load, which may be one of\n"
        "                     the real ones\n"
        "  --class <guid>     Which kind of device to ask it for\n"
        "\n"
        "What the device asks for on the way up is printed. That list is what\n"
        "has to exist before the real hardware will run against this.\n");
}

int main(int argc, char **argv)
{
    const char *Library = nullptr;
    const char *Class = nullptr;
    const char *Firmware = nullptr;
    ULONG Steps = 20000;
    ULONG64 Ram = 0;

    /*
     * Said the moment it is said. What is being driven here is somebody else's
     * and may stop the whole process rather than return, and anything still
     * waiting to be written when that happens is the part worth reading.
     */
    setvbuf(stdout, nullptr, _IONBF, 0);

    for (int Index = 1; Index < argc; Index++)
    {
        if ((strcmp(argv[Index], "--library") == 0) && ((Index + 1) < argc))
            Library = argv[++Index];
        else if ((strcmp(argv[Index], "--class") == 0) && ((Index + 1) < argc))
            Class = argv[++Index];
        else if (strcmp(argv[Index], "--store-refuses") == 0)
            RepositoryAnswer = E_NOTIMPL;
        else if ((strcmp(argv[Index], "--firmware") == 0) && ((Index + 1) < argc))
            Firmware = argv[++Index];
        else if ((strcmp(argv[Index], "--steps") == 0) && ((Index + 1) < argc))
            Steps = (ULONG)strtoul(argv[++Index], nullptr, 0);
        else if ((strcmp(argv[Index], "--memory") == 0) && ((Index + 1) < argc))
            Ram = strtoull(argv[++Index], nullptr, 0);
        else
        {
            Usage();
            return 1;
        }
    }

    if (Firmware != nullptr)
        return hv::Run(Firmware, Steps, Ram);

    if ((Library == nullptr) || (Class == nullptr))
    {
        Usage();
        return 1;
    }

    GUID Wanted = {};

    if (!ReadGuid(Class, Wanted))
    {
        printf("%s is not written the way a kind is named\n", Class);
        return 1;
    }

    /*
     * Loaded the way it expects to be. A class server of this kind pulls in
     * more of the machine it came from as it goes, and anything it cannot find
     * is a piece of that machine which is not here.
     */
    const HMODULE Loaded = LoadLibraryA(Library);

    if (Loaded == nullptr)
    {
        printf("%s would not load, %lu\n", Library, GetLastError());
        return 1;
    }

    printf("%s is loaded\n", Library);

    auto GetClassObject = reinterpret_cast<PFN_GET_CLASS_OBJECT>(
        reinterpret_cast<void *>(GetProcAddress(Loaded, "DllGetClassObject")));

    if (GetClassObject == nullptr)
    {
        printf("it hands out nothing, so it is not a class server\n");
        return 1;
    }

    IClassFactory *Factory = nullptr;
    HRESULT Status = GetClassObject(Wanted, IID_IClassFactory,
                                    reinterpret_cast<void **>(&Factory));

    if (FAILED(Status))
    {
        printf("it has no such kind, %08lx\n", Status);
        return 1;
    }

    printf("it offers that kind\n");

    IVirtualDevice *Device = nullptr;

    Status = Factory->CreateInstance(nullptr, IID_IVirtualDevice,
                                     reinterpret_cast<void **>(&Device));
    Factory->Release();

    if (FAILED(Status))
    {
        printf("it would not make one, %08lx\n", Status);
        printf("which says the identifier for what every device is does not "
               "agree\n");
        return 1;
    }

    printf("one was made, so the identifier for what every device is agrees\n");

    /* What it says it cannot come up without, before it is asked for anything */
    ULONG Count = 0;
    GUID *Services = nullptr;
    ULONG Required = 0;

    Status = Device->GetDependencies(nullptr, &Count, &Services, &Required);

    if (SUCCEEDED(Status) && (Services != nullptr))
    {
        printf("it names %lu service(s):\n", Count);

        for (ULONG Index = 0; Index < Count; Index++)
        {
            printf("  %2lu %s ", Index,
                   (Index < Required) ? "must have" : "may have");
            SayGuid(Services[Index]);
            printf("\n");
        }

        CoTaskMemFree(Services);
    }
    else
    {
        printf("it would not say what it needs, %08lx\n", Status);
    }

    /* And then what it actually goes looking for once it is brought up */
    Probe Handed;

    printf("bringing it up:\n");

    const HRESULT Came = Device->Initialize(&TheRepository, 0,
                                            static_cast<IUnknown *>(&Handed));

    printf("it came up with %08lx after asking for %lu\n", Came,
           Handed.Asked());

    /*
     * And then the rest of the way up. Reserving comes first and on most kinds
     * does nothing but move a state along; the addresses are asked for when the
     * machine is switched on, which is why that step is here and not skipped.
     */
    printf("asking where it answers:\n");
    Status = Device->StartReservingResources(&TheRepository, VDEV_STATE_NONE);
    printf("it started with %08lx\n", Status);

    if (SUCCEEDED(Status))
    {
        Status = Device->FinishReservingResources(VDEV_STATE_NONE);
        printf("and settled with %08lx\n", Status);
    }

    /*
     * Only if it came up. A device that was refused something it said it could
     * not do without has nothing to switch on, and most of them go straight at
     * whatever they were refused rather than checking again.
     */
    if (FAILED(Came))
    {
        printf("it was refused something it must have, so it is not driven\n");
        Device->Teardown();
        Device->Release();
        return 0;
    }

    printf("switching it on:\n");
    Status = Device->PowerOnCold(VDEV_STATE_NONE);
    printf("it came on with %08lx, having asked for %lu run(s) of ports\n",
           Status, Handed.Addresses().Ports());

    /*
     * And then whether it answers. A device that hands back what its kind has
     * always had, for a port it asked for itself, is running: nothing short of
     * every slot in every interface being right gets this far.
     */
    Drive(Handed, Wanted);

    if (Handed.Wires().Raised() != 0)
        printf("and it raised its line %lu time(s)\n", Handed.Wires().Raised());

    Device->PowerOff(VDEV_STATE_NONE);
    Device->Teardown();
    Device->Release();
    return 0;
}
