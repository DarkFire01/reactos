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
#include <winhvplatform.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vdev.h"
#include "vm.h"

/* Where the machine itself is, which is the other half of this program */
namespace hv
{

int Run(const char *Path, ULONG Steps, ULONG64 Ram);

bool Open(ULONG64 Ram);
bool Hollow(ULONG64 Where, ULONG64 Length);
bool Place(const void *Image, ULONG Length, ULONG64 Where);
bool Fill(ULONG64 Where, ULONG64 Length, void *Backing, bool ReadOnly);
bool Reachable(ULONG64 Address, ULONG64 *Physical);
bool Start(USHORT Selector, ULONG64 Base, ULONG64 Rip);
bool Step(WHV_RUN_VP_EXIT_CONTEXT *Exit);
bool Poke(WHV_REGISTER_NAME Which, ULONG64 Value);
ULONG64 Peek(WHV_REGISTER_NAME Which);
void *Guest(ULONG64 Where, ULONG Length);
void Tell();
void Close();

} /* namespace hv */

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

/* What it is called, or nothing if it has no name here */
static const char *NameOf(const GUID &Which)
{
    for (const auto &Known : KnownNames)
    {
        if (IsEqualGUID(Which, *Known.Which))
            return Known.Name;
    }

    return "something unnamed";
}

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
class Emulation;

class Reservation : public IVndRegistration
{
public:
    Reservation(Emulation *Where, const void *What) noexcept
        : m_Where(Where), m_What(What) {}

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

    STDMETHODIMP Revoke() override;

private:
    ~Reservation() = default;

    Emulation *m_Where = nullptr;
    const void *m_What = nullptr;

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
            *Registration = new Reservation(this, Handler);

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
            m_Port[m_Ports].Held = true;
        }

        m_Ports++;

        if (Registration != nullptr)
            *Registration = new Reservation(this, Handler);

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

        /* Whether it is still the device.s, or was given back */
        bool Held;
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
            if (!m_Port[Index].Held)
                continue;

            if ((Port >= m_Port[Index].First) && (Port <= m_Port[Index].Last))
                return m_Port[Index].Handler;
        }

        return nullptr;
    }

    /*
     * A range given back. It has to stop being dispatched to at once: a device
     * whose coming up failed part way through gives back what it had already
     * taken, and answering for it afterwards is a call into something that is
     * about to be freed.
     */
    void Forget(const void *Handler) noexcept
    {
        for (ULONG Index = 0; Index < Kept(); Index++)
        {
            if (m_Port[Index].Handler == Handler)
                m_Port[Index].Held = false;
        }
    }

private:
    volatile LONG m_Count = 1;

    Reserved m_Port[32] = {};
    ULONG m_Ports = 0;
};

/* Told once the device that reserved it has finished with it */
STDMETHODIMP Reservation::Revoke()
{
    printf("    gives a range back\n");

    if (m_Where != nullptr)
        m_Where->Forget(m_What);

    return S_OK;
}

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
        if (Second == nullptr)
        {
            printf("      about: nothing\n");
            return RepositoryAnswer;
        }

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
 * Which version of its own contract a device is being driven through.
 *
 * It names the oldest and newest it can work to, and what is written back is
 * the one it is going to get. A device keeps that and behaves differently by
 * it, so the newest it offers is the honest answer from something that has
 * only ever been written against the newest.
 */
static HRESULT STDMETHODCALLTYPE RepositoryVersion(void *This, ULONG64 Least,
                                                   ULONG64 Most, ULONG *Chosen)
{
    UNREFERENCED_PARAMETER(This);
    UNREFERENCED_PARAMETER(Least);

    if (Chosen != nullptr)
        *Chosen = (ULONG)Most;

    if (!RepositoryQuiet)
    {
        printf("    store: asked which version, between %llu and %llu\n",
               (unsigned long long)Least, (unsigned long long)Most);
    }

    return S_OK;
}

/*
 * What a device is told its configuration is.
 *
 * It is XML, in UTF-16, and the device reads it with the system's own reader.
 * A device is not handed a string: it hands over somewhere to write into and
 * the configuration is written there, which is why this takes a stream.
 *
 * What the elements in it are called is each kind's own business and is not
 * written down anywhere outside its own parser. An empty document is well
 * formed, so a kind whose settings are all optional comes up on it, and one
 * whose are not says so, which is how the rest of them get found out.
 */
static const char *TheConfiguration = nullptr;

/*
 * Which kind is being fitted, and whatever it was told to take for itself.
 *
 * The configuration is asked for without saying which device is asking, so what
 * says that is which one is being brought up at the time.
 */
static const char *TheFitting = "?";
static const char *TheFittingSettings = nullptr;

/* The kinds whose configuration is known, by the identifier they answer to */
#define CLASS_VIDEO "7d80d3db-61ee-4879-8879-5609f1100ad0"
#define CLASS_IDE   "83f8638b-8dca-4152-9eda-2ca8b33039b4"

/*
 * What one kind is told.
 *
 * Every one of these begins with the two numbers the schema of any device
 * carries, and then whatever that kind has of its own. The display wants an
 * address written the way it scans it back, which is four hex digits that have
 * to be one of two particular values, then eight, then two. The disk controller
 * wants a drive inside a controller, and the drive is where the disc goes.
 */
static const char *ConfigurationFor(const char *Class, const char *Settings)
{
    static char Made[2048];

    if (TheConfiguration != nullptr)
        return TheConfiguration;

    if ((Class != nullptr) && (_stricmp(Class, CLASS_VIDEO) == 0))
    {
        return "<VDEVVersion>512</VDEVVersion>"
               "<version>2</version>"
               "<address>5353,00000000,00</address>";
    }

    if ((Class != nullptr) && (_stricmp(Class, CLASS_IDE) == 0))
    {
        /* A controller with nothing in it, for a machine with no disc in it */
        if ((Settings == nullptr) || (Settings[0] == '\0'))
        {
            return "<VDEVVersion>512</VDEVVersion>"
                   "<version>2</version>"
                   "<controller/>";
        }

        /*
         * A controller is a channel and both of them repeat, so the first
         * controller is the primary pair and the second the secondary, and
         * the drives inside each are master then slave. Nothing says which is
         * which: the order is the whole of it.
         *
         * One on each, because which channel a firmware looks at for a disc is
         * its own business and they do not agree about it.
         */
        _snprintf(Made, sizeof(Made) - 1,
                  "<VDEVVersion>512</VDEVVersion>"
                  "<version>2</version>"
                  "<controller>"
                  "<drive><pathname>%s</pathname><type>ISO</type></drive>"
                  "</controller>"
                  "<controller>"
                  "<drive><pathname>%s</pathname><type>ISO</type></drive>"
                  "</controller>",
                  Settings, Settings);

        Made[sizeof(Made) - 1] = '\0';
        return Made;
    }

    /* And nothing at all for a kind whose settings are all its own defaults */
    return "";
}

static HRESULT STDMETHODCALLTYPE RepositoryConfiguration(void *This,
                                                         ISequentialStream *Writer,
                                                         ULONG *Answered)
{
    UNREFERENCED_PARAMETER(This);

    if (Answered != nullptr)
        *Answered = 0;

    if (Writer == nullptr)
        return E_POINTER;

    const char *Xml = ConfigurationFor(TheFitting, TheFittingSettings);

    if (Xml[0] == 0)
        return S_OK;

    WCHAR Wide[4096];
    const int Length = MultiByteToWideChar(CP_ACP, 0, Xml, -1,
                                           Wide, ARRAYSIZE(Wide));

    if (Length <= 1)
        return E_FAIL;

    /* Counted in bytes, and without the end of the string, which is not text */
    ULONG Written = 0;
    const HRESULT Status = Writer->Write(Wide,
                                         (ULONG)((Length - 1) * sizeof(WCHAR)),
                                         &Written);

    /*
     * And what it made of it. The thing written into is a string with the text
     * in it, and what the device does next is take that string straight back
     * out, so a write that said it worked and left the string empty is the one
     * failure that would otherwise look exactly like a bad document.
     */
    if (!RepositoryQuiet)
    {
        const ULONG64 *Held = static_cast<const ULONG64 *>(
            static_cast<void *>(Writer));

        printf("    it was told its configuration, %08lx, %lu of %lu byte(s)\n",
               Status, Written, (ULONG)((Length - 1) * sizeof(WCHAR)));
        printf("    and what it is holding is %llu long\n",
               (unsigned long long)Held[4]);
    }

    return Status;
}

void VmConfiguration(const char *Xml)
{
    if (Xml != nullptr)
        TheConfiguration = Xml;
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
    SLOT(4),  SLOT(5),  SLOT(6),
    reinterpret_cast<const void *>(&RepositoryVersion),
    SLOT(8),  SLOT(9),  SLOT(10),
    reinterpret_cast<const void *>(&RepositoryName),
    SLOT(12), SLOT(13), SLOT(14), SLOT(15),
    SLOT(16), SLOT(17), SLOT(18), SLOT(19),
    SLOT(20), SLOT(21), SLOT(22), SLOT(23),
    SLOT(24), SLOT(25), SLOT(26), SLOT(27),
    SLOT(28), SLOT(29), SLOT(30), SLOT(31),
    SLOT(32), SLOT(33), SLOT(34), SLOT(35),
    SLOT(36), SLOT(37), SLOT(38), SLOT(39),
    reinterpret_cast<const void *>(&RepositoryConfiguration),
    SLOT(41), SLOT(42), SLOT(43),
    SLOT(44), SLOT(45), SLOT(46), SLOT(47)
};

static RepositoryObject TheRepository = { RepositoryVtable, 1 };

/* THE CLOCK ******************************************************************/

/*
 * One timer, kept by whichever device asked for it.
 *
 * Nothing here runs on a thread. A timer that has gone off is noticed by the
 * machine between one stop of the processor and the next, which is late but is
 * never early and never comes in the middle of something.
 */
class Timer : public IVmTimer
{
public:
    Timer(IVmTimerHandler *Handler, const char *Whose) noexcept
        : m_Handler(Handler), m_Whose(Whose)
    {
        if (m_Handler != nullptr)
            m_Handler->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        UNREFERENCED_PARAMETER(Interface);

        if (Object == nullptr)
            return E_POINTER;

        *Object = static_cast<IVmTimer *>(this);
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_Count);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        return (ULONG)InterlockedDecrement(&m_Count);
    }

    STDMETHODIMP GetTime(ULONG64 *Now) override
    {
        if (Now == nullptr)
            return E_POINTER;

        *Now = Ticks();
        return S_OK;
    }

    STDMETHODIMP Reserved4() override { return E_NOTIMPL; }
    STDMETHODIMP Reserved5() override { return E_NOTIMPL; }
    STDMETHODIMP Reserved6() override { return E_NOTIMPL; }
    STDMETHODIMP Reserved7() override { return E_NOTIMPL; }

    STDMETHODIMP Arm(ULONG64 Kind, ULONG64 Period, ULONG64 Due,
                     ULONG Repeating) override
    {
        UNREFERENCED_PARAMETER(Kind);

        m_Period = Period;
        m_Due = Due;
        m_Repeating = (Repeating != 0);
        m_Armed = true;
        m_Arms++;

        return S_OK;
    }

    STDMETHODIMP Reserved9() override { return E_NOTIMPL; }
    STDMETHODIMP Reserved10() override { return E_NOTIMPL; }

    STDMETHODIMP Cancel(ULONG64 Why) override
    {
        UNREFERENCED_PARAMETER(Why);

        m_Armed = false;
        return S_OK;
    }

    /* Goes off if it is due, and sets itself again if that is what was asked */
    bool Ring()
    {
        if (!m_Armed || (Ticks() < m_Due))
            return false;

        if (m_Repeating && (m_Period != 0))
            m_Due += m_Period;
        else
            m_Armed = false;

        m_Rings++;

        if (m_Handler != nullptr)
            m_Handler->OnTimerExpired(static_cast<IVmTimer *>(this));

        return true;
    }

    const char *Whose() const noexcept { return m_Whose; }
    ULONG Arms() const noexcept { return m_Arms; }
    ULONG Rings() const noexcept { return m_Rings; }

private:
    static ULONG64 Ticks()
    {
        LARGE_INTEGER Now = {};
        LARGE_INTEGER Rate = {};

        QueryPerformanceCounter(&Now);
        QueryPerformanceFrequency(&Rate);

        if (Rate.QuadPart == 0)
            return 0;

        return ((ULONG64)Now.QuadPart * VDEV_TICKS_A_SECOND) /
               (ULONG64)Rate.QuadPart;
    }

    volatile LONG m_Count = 1;
    IVmTimerHandler *m_Handler = nullptr;
    const char *m_Whose = "?";

    ULONG64 m_Period = 0;
    ULONG64 m_Due = 0;
    bool m_Repeating = false;
    bool m_Armed = false;
    ULONG m_Arms = 0;
    ULONG m_Rings = 0;
};

/* As many as the devices in one machine ask for between them */
static Timer *TheTimers[16];
static ULONG TheTimerCount = 0;

/*
 * Where a device gets one. Which device is asking is not said anywhere in the
 * call, so what it is called is taken from whichever one is being fitted.
 */

class Clock : public IVmTimeSource
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IVmTimeSource))
        {
            *Object = static_cast<IVmTimeSource *>(this);
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

    STDMETHODIMP Reserved3() override
    {
        printf("    the clock was asked something at slot 3\n");
        return E_NOTIMPL;
    }

    STDMETHODIMP Reserved4() override
    {
        printf("    the clock was asked something at slot 4\n");
        return E_NOTIMPL;
    }

    STDMETHODIMP CreateTimer(IVmTimerHandler *Handler,
                             IVmTimer **Made) override
    {
        if (Made == nullptr)
            return E_POINTER;

        *Made = nullptr;

        if (TheTimerCount >= ARRAYSIZE(TheTimers))
            return E_OUTOFMEMORY;

        auto *One = new Timer(Handler, TheFitting);

        if (One == nullptr)
            return E_OUTOFMEMORY;

        TheTimers[TheTimerCount++] = One;
        *Made = static_cast<IVmTimer *>(One);

        printf("    a timer, which makes %lu\n", TheTimerCount);
        return S_OK;
    }

private:
    volatile LONG m_Count = 1;
};

/* Every timer that is due, called back. What comes of that is the device's */
static ULONG Ring()
{
    ULONG Rang = 0;

    for (ULONG Index = 0; Index < TheTimerCount; Index++)
    {
        if (TheTimers[Index]->Ring())
            Rang++;
    }

    return Rang;
}

/* A SERVICE NOBODY HAS LAID OUT YET ******************************************/

/*
 * Something to hand a device that asks for a service this does not have.
 *
 * A device that is refused one of these stops on the spot, and a device that
 * stops says nothing further about itself. Handing it one of these instead gets
 * it all the way up, and then every call it makes into the thing it was given
 * is written down: which slot, and what was passed. That is the same way the
 * slots that are known here were worked out in the first place.
 *
 * Nothing is answered properly. A slot that was supposed to write something
 * back does not, so the device reads whatever was already there. A device that
 * goes wrong right after one of these is named has been told a lie by it, and
 * the log says which lie.
 */
#define STRANGER_SLOTS 64

struct StrangerObject
{
    const void **Vtable;
    volatile LONG Count;
    const char *Name;
};

static HRESULT StrangerAnswer = S_OK;

template <int Slot>
static HRESULT STDMETHODCALLTYPE StrangerSlot(void *This, void *First,
                                              void *Second, void *Third,
                                              void *Fourth)
{
    const auto *Self = static_cast<const StrangerObject *>(This);

    printf("    %s slot %d (%p %p %p %p)\n",
           (Self != nullptr) ? Self->Name : "?", Slot,
           First, Second, Third, Fourth);

    return StrangerAnswer;
}

/*
 * Answered for anything at all. A device that asks one of these whether it is
 * also something else is asking a question this cannot answer, and saying yes
 * keeps it going where saying no would stop it.
 */
static HRESULT STDMETHODCALLTYPE StrangerQuery(void *This, REFIID Interface,
                                              void **Object)
{
    auto *Self = static_cast<StrangerObject *>(This);

    if (Object == nullptr)
        return E_POINTER;

    printf("    %s asked whether it is also ", Self->Name);
    SayGuid(Interface);
    printf("\n");

    InterlockedIncrement(&Self->Count);
    *Object = Self;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE StrangerHold(void *This)
{
    auto *Self = static_cast<StrangerObject *>(This);

    return (ULONG)InterlockedIncrement(&Self->Count);
}

static ULONG STDMETHODCALLTYPE StrangerDrop(void *This)
{
    auto *Self = static_cast<StrangerObject *>(This);

    return (ULONG)InterlockedDecrement(&Self->Count);
}

#undef SLOT
#define SLOT(n) reinterpret_cast<const void *>(&StrangerSlot<n>)

static const void *StrangerVtable[STRANGER_SLOTS] =
{
    reinterpret_cast<const void *>(&StrangerQuery),
    reinterpret_cast<const void *>(&StrangerHold),
    reinterpret_cast<const void *>(&StrangerDrop),
    SLOT(3),  SLOT(4),  SLOT(5),  SLOT(6),  SLOT(7),
    SLOT(8),  SLOT(9),  SLOT(10), SLOT(11), SLOT(12), SLOT(13),
    SLOT(14), SLOT(15), SLOT(16), SLOT(17), SLOT(18), SLOT(19),
    SLOT(20), SLOT(21), SLOT(22), SLOT(23), SLOT(24), SLOT(25),
    SLOT(26), SLOT(27), SLOT(28), SLOT(29), SLOT(30), SLOT(31),
    SLOT(32), SLOT(33), SLOT(34), SLOT(35), SLOT(36), SLOT(37),
    SLOT(38), SLOT(39), SLOT(40), SLOT(41), SLOT(42), SLOT(43),
    SLOT(44), SLOT(45), SLOT(46), SLOT(47), SLOT(48), SLOT(49),
    SLOT(50), SLOT(51), SLOT(52), SLOT(53), SLOT(54), SLOT(55),
    SLOT(56), SLOT(57), SLOT(58), SLOT(59), SLOT(60), SLOT(61),
    SLOT(62), SLOT(63)
};

/* One for each, so that what is written down says which service it was */
static StrangerObject TheStrangers[24];
static ULONG TheStrangerCount = 0;

/* Whether a service this does not have is stood in for at all */
static bool StrangersAllowed = false;

static void *StandIn(REFIID Service)
{
    if (!StrangersAllowed || (TheStrangerCount >= ARRAYSIZE(TheStrangers)))
        return nullptr;

    StrangerObject *Self = &TheStrangers[TheStrangerCount++];

    Self->Vtable = StrangerVtable;
    Self->Count = 1;
    Self->Name = NameOf(Service);

    return Self;
}


/* WHAT THE OPERATOR IS LOOKING AT ********************************************/

/*
 * The display, as the video device sees it.
 *
 * It is not handed a screen. It is told which part of one stopped being what it
 * was, and it is the front end's business to come back and fetch as much of
 * that as it wants to draw, whenever it next has time to.
 */
/*
 * A block of memory a device was given, as that device sees it.
 *
 * What the display device does with the one it is handed is not yet known: it
 * wraps it in something of its own and asks that to watch for writes. So this
 * answers nothing and writes down every slot it is asked for, which is how the
 * rest of it gets found out.
 */
#define BLOCK_SLOTS 32

struct BlockObject
{
    const void **Vtable;
    volatile LONG Count;

    /* What the block stands for, which is memory on this side */
    UCHAR *Memory;
    ULONG64 Length;
};

template <int Slot>
static HRESULT STDMETHODCALLTYPE BlockSlot(void *This, void *First,
                                           void *Second, void *Third)
{
    UNREFERENCED_PARAMETER(This);

    if (!RepositoryQuiet)
    {
        printf("    the vram block was asked for slot %d (%p %p %p)\n",
               Slot, First, Second, Third);
    }

    return S_OK;
}

/*
 * A run of the block, made reachable and handed back as an address.
 *
 * This is how a device gets at the memory it was given: it names a run of pages
 * within the block and is told where that run is. The display asks for eight
 * pages of it under a name of its own and writes the screen straight in.
 */
static HRESULT STDMETHODCALLTYPE BlockAperture(void *This, ULONG64 Offset,
                                               ULONG64 Pages, ULONG Flags,
                                               const WCHAR *Name,
                                               void **Mapped, void **Handle)
{
    auto *Self = static_cast<BlockObject *>(This);
    const ULONG64 Where = Offset * VDEV_PAGE_SIZE;
    const ULONG64 Length = Pages * VDEV_PAGE_SIZE;

    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Name);

    if ((Self->Memory == nullptr) || ((Where + Length) > Self->Length))
        return E_INVALIDARG;

    if (Mapped != nullptr)
        *Mapped = Self->Memory + Where;

    /* Something to give the run up by, which is the block it came out of */
    if (Handle != nullptr)
        *Handle = Self;

    if (!RepositoryQuiet)
    {
        printf("    %llu page(s) of the block, from %llu, are at %p\n",
               (unsigned long long)Pages, (unsigned long long)Offset,
               (void *)(Self->Memory + Where));
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BlockQuery(void *This, REFIID Interface,
                                            void **Object)
{
    UNREFERENCED_PARAMETER(Interface);

    if (Object == nullptr)
        return E_POINTER;

    InterlockedIncrement(&static_cast<BlockObject *>(This)->Count);
    *Object = This;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE BlockHold(void *This)
{
    return (ULONG)InterlockedIncrement(&static_cast<BlockObject *>(This)->Count);
}

static ULONG STDMETHODCALLTYPE BlockDrop(void *This)
{
    return (ULONG)InterlockedDecrement(&static_cast<BlockObject *>(This)->Count);
}

#undef SLOT
#define SLOT(n) reinterpret_cast<const void *>(&BlockSlot<n>)

static const void *BlockVtable[BLOCK_SLOTS] =
{
    reinterpret_cast<const void *>(&BlockQuery),
    reinterpret_cast<const void *>(&BlockHold),
    reinterpret_cast<const void *>(&BlockDrop),
    SLOT(3),
    reinterpret_cast<const void *>(&BlockAperture),
    SLOT(5),  SLOT(6),  SLOT(7),
    SLOT(8),  SLOT(9),  SLOT(10), SLOT(11), SLOT(12), SLOT(13),
    SLOT(14), SLOT(15), SLOT(16), SLOT(17), SLOT(18), SLOT(19),
    SLOT(20), SLOT(21), SLOT(22), SLOT(23), SLOT(24), SLOT(25),
    SLOT(26), SLOT(27), SLOT(28), SLOT(29), SLOT(30), SLOT(31)
};

/* As many as the devices in one machine ask for between them */
static BlockObject TheBlocks[8];
static ULONG TheBlockCount = 0;

static BlockObject TheVram = { BlockVtable, 1, nullptr, 0 };

/* One, with memory behind it for whatever asked to be given it */
static IVmMemoryBlock *MakeBlock(ULONG64 Length)
{
    if (TheBlockCount >= ARRAYSIZE(TheBlocks))
        return nullptr;

    BlockObject *One = &TheBlocks[TheBlockCount];

    One->Vtable = BlockVtable;
    One->Count = 1;
    One->Length = Length;
    One->Memory = static_cast<UCHAR *>(VirtualAlloc(nullptr, (SIZE_T)Length,
                                                    MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE));

    if (One->Memory == nullptr)
        return nullptr;

    TheBlockCount++;
    return reinterpret_cast<IVmMemoryBlock *>(One);
}

/*
 * What the operator is looking at, as a display device works against it.
 *
 * The memory the picture lives in is this side's, not the device's. The device
 * says how much of it the card is supposed to have, asks for it, and from then
 * on writes the screen into it directly; what draws a window from it is another
 * matter entirely and happens on whichever thread has time for it.
 */
class Monitor : public IMonitorDevice
{
public:
    Monitor() { InitializeCriticalSection(&m_Lock); }

    ~Monitor()
    {
        if (m_Vram != nullptr)
            VirtualFree(m_Vram, 0, MEM_RELEASE);

        DeleteCriticalSection(&m_Lock);
    }

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IMonitorDevice))
        {
            *Object = static_cast<IMonitorDevice *>(this);
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

    STDMETHODIMP GetThumbnailImage(void *Repository, USHORT Width,
                                   USHORT Height, ULONG Flags,
                                   void **Image) override
    {
        UNREFERENCED_PARAMETER(Repository);
        UNREFERENCED_PARAMETER(Width);
        UNREFERENCED_PARAMETER(Height);
        UNREFERENCED_PARAMETER(Flags);

        if (Image != nullptr)
            *Image = nullptr;

        return E_NOTIMPL;
    }

    STDMETHODIMP RequestBitmap(RECT Where, ULONG Pitch, int Depth,
                               UCHAR *Pixels, ULONG Length, int Which) override
    {
        UNREFERENCED_PARAMETER(Where);
        UNREFERENCED_PARAMETER(Pitch);
        UNREFERENCED_PARAMETER(Depth);
        UNREFERENCED_PARAMETER(Pixels);
        UNREFERENCED_PARAMETER(Length);
        UNREFERENCED_PARAMETER(Which);
        return E_NOTIMPL;
    }

    /*
     * Made the first time it is asked for, at whatever size was last said. It
     * is committed rather than reserved because the device writes into it the
     * moment it has the address and never asks again.
     */
    STDMETHODIMP GetVramBaseAddress(UCHAR **Base) override
    {
        if (Base == nullptr)
            return E_POINTER;

        *Base = Allocate();

        if (!RepositoryQuiet)
            printf("    the vram is at %p\n", (void *)*Base);

        return (*Base != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetVramSize(ULONG *Size) override
    {
        if (Size == nullptr)
            return E_POINTER;

        *Size = m_Wanted;
        return S_OK;
    }

    STDMETHODIMP GetVramMemoryBlock(IVmMemoryBlock **Block) override
    {
        if (Block == nullptr)
            return E_POINTER;

        Allocate();
        InterlockedIncrement(&TheVram.Count);
        *Block = reinterpret_cast<IVmMemoryBlock *>(&TheVram);

        if (!RepositoryQuiet)
            printf("    the vram was asked for as a block\n");

        return S_OK;
    }

    STDMETHODIMP IsVramAllocated() override
    {
        return (m_Vram != nullptr) ? S_OK : S_FALSE;
    }

    STDMETHODIMP ClearVram() override
    {
        if (m_Vram != nullptr)
            memset(m_Vram, 0, m_Wanted);

        return S_OK;
    }

    STDMETHODIMP SetMemoryRequired(ULONG Bytes) override
    {
        if (!RepositoryQuiet)
            printf("    the card is to have %lu byte(s) of vram\n", Bytes);

        m_Wanted = Bytes;
        return S_OK;
    }

    STDMETHODIMP SetMemoryForSave(ULONG Bytes) override
    {
        UNREFERENCED_PARAMETER(Bytes);
        return S_OK;
    }

    /* Kept, because it is the only way back to the screen that is drawn from */
    STDMETHODIMP RegisterVideoSource(IVideoVdev *Display, int Which) override
    {
        UNREFERENCED_PARAMETER(Which);

        if (!RepositoryQuiet)
            printf("    a display offered itself to be drawn\n");

        m_Display = Display;
        return S_OK;
    }

    STDMETHODIMP GetDisplaySettings(ULONG *Width, ULONG *Height,
                                    ULONG *Depth) override
    {
        if ((Width == nullptr) || (Height == nullptr) || (Depth == nullptr))
            return E_POINTER;

        *Width = m_Width;
        *Height = m_Height;
        *Depth = m_Depth;
        return S_OK;
    }

    STDMETHODIMP GetPointerPosition(int *Across, int *Down) override
    {
        if ((Across == nullptr) || (Down == nullptr))
            return E_POINTER;

        *Across = 0;
        *Down = 0;
        return S_OK;
    }

    STDMETHODIMP GetPointerShape(void *Shape) override
    {
        UNREFERENCED_PARAMETER(Shape);
        return E_NOTIMPL;
    }

    STDMETHODIMP GetActiveDeviceType(VDEV_VIDEO_KIND *Which) override
    {
        if (Which == nullptr)
            return E_POINTER;

        *Which = VDEV_VIDEO_S3;
        return S_OK;
    }

    /* One, because a machine nobody is watching need not draw anything */
    STDMETHODIMP GetClientCount(ULONG *Count) override
    {
        if (Count == nullptr)
            return E_POINTER;

        *Count = 1;
        return S_OK;
    }

    STDMETHODIMP SetMonitorVideoActive(int Active, ULONG Which) override
    {
        UNREFERENCED_PARAMETER(Which);

        if (!RepositoryQuiet)
            printf("    the picture is %s\n", Active ? "on" : "off");

        return S_OK;
    }

    STDMETHODIMP RegisterSyntheticMouse(IVmMouseDevice *Mouse) override
    {
        UNREFERENCED_PARAMETER(Mouse);
        return S_OK;
    }

    STDMETHODIMP OnClientCountChanged() override { return S_OK; }
    STDMETHODIMP OnDisplaySettingsChanged() override { return S_OK; }

    /* What the front end draws from, and how big it is */
    const UCHAR *Vram() const noexcept { return m_Vram; }
    ULONG VramSize() const noexcept { return m_Wanted; }
    IVideoVdev *Display() const noexcept { return m_Display; }

    /* Told by our own devices, which say so rather than leaving it to be found */
    void Dirty(VDEV_VIDEO_KIND Which)
    {
        EnterCriticalSection(&m_Lock);
        m_Dirty = true;
        m_Which = Which;
        LeaveCriticalSection(&m_Lock);
    }

    bool Take(VDEV_VIDEO_KIND *Which)
    {
        EnterCriticalSection(&m_Lock);

        const bool Was = m_Dirty;

        if (Was && (Which != nullptr))
            *Which = m_Which;

        m_Dirty = false;

        LeaveCriticalSection(&m_Lock);
        return Was;
    }

private:
    UCHAR *Allocate()
    {
        if ((m_Vram == nullptr) && (m_Wanted != 0))
        {
            m_Vram = static_cast<UCHAR *>(VirtualAlloc(nullptr, m_Wanted,
                                                       MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_READWRITE));
        }

        return m_Vram;
    }

    volatile LONG m_Count = 1;
    CRITICAL_SECTION m_Lock = {};

    UCHAR *m_Vram = nullptr;
    ULONG m_Wanted = 0;
    IVideoVdev *m_Display = nullptr;

    ULONG m_Width = 640;
    ULONG m_Height = 480;
    ULONG m_Depth = 32;

    VDEV_VIDEO_KIND m_Which = 0;
    bool m_Dirty = false;
};

/* THE FIRMWARE, AS A DEVICE IN IT SEES IT ************************************/

/*
 * What a device asks the firmware for.
 *
 * On a real machine of this kind the firmware loader offers this, and the
 * clock, the display and the disk controller all want it. There is no firmware
 * loader here, so the answers are the host's: they are the answers a machine
 * with nothing unusual about it would give, which is what every one of these
 * devices is expecting anyway.
 */
class Bios : public IVmBios
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IVmBios))
        {
            *Object = static_cast<IVmBios *>(this);
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

    STDMETHODIMP NotifyEmulatedActivity() override
    {
        m_Activity++;
        return S_OK;
    }

    STDMETHODIMP RegisterBootDevice(ULONG Kind) override
    {
        if (!RepositoryQuiet)
            printf("    a device of kind %lu says it can be booted from\n", Kind);

        m_Bootable++;
        return S_OK;
    }

    STDMETHODIMP EnableSerialController(UCHAR Which) override
    {
        if (!RepositoryQuiet)
            printf("    serial controller %u says it is there\n", Which);

        return S_OK;
    }

    /*
     * Nothing, which is what a clock that has never been set holds. What the
     * time actually is comes from the clock reading the host when it starts,
     * not from here: this is only the part a firmware would have written.
     */
    STDMETHODIMP GetDefaultCmosValues(UCHAR *Values) override
    {
        if (Values == nullptr)
            return E_POINTER;

        memset(Values, 0, VDEV_CMOS_DEFAULTS);
        return S_OK;
    }

    STDMETHODIMP IsGuestHibernateEnabled(int *Enabled) override
    {
        if (Enabled == nullptr)
            return E_POINTER;

        *Enabled = 0;
        return S_OK;
    }

    STDMETHODIMP SaveShutdownType(ULONG Why) override
    {
        if (!RepositoryQuiet)
            printf("    the machine is stopping, for %lu\n", Why);

        return S_OK;
    }

    ULONG Bootable() const noexcept { return m_Bootable; }

private:
    volatile LONG m_Count = 1;
    ULONG m_Activity = 0;
    ULONG m_Bootable = 0;
};

/* THE GUEST'S MEMORY, AS A DEVICE THAT REACHES INTO IT SEES IT ***************/

/*
 * A window of the guest's memory whose contents belong to a device.
 *
 * What is behind it is the device's own buffer, mapped into the guest where the
 * device asked for it. Nothing stops to ask the device about an access to it
 * afterwards, which is the entire point: a screen of text is written to at the
 * speed of memory instead of one stop of the processor per character.
 */
class Window : public IVndRegistration
{
public:
    Window(ULONG64 Where, ULONG64 Length) noexcept
        : m_Where(Where), m_Length(Length) {}

    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        UNREFERENCED_PARAMETER(Interface);

        if (Object == nullptr)
            return E_POINTER;

        *Object = static_cast<IVndRegistration *>(this);
        AddRef();
        return S_OK;
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
        if (m_Length != 0)
        {
            hv::Hollow(m_Where, m_Length);
            m_Length = 0;
        }

        return S_OK;
    }

private:
    ~Window() = default;

    ULONG64 m_Where = 0;
    ULONG64 m_Length = 0;
    volatile LONG m_Count = 1;
};

class GuestMemory : public IVmGuestMemoryAccess
{
public:
    STDMETHODIMP QueryInterface(REFIID Interface, void **Object) override
    {
        if (Object == nullptr)
            return E_POINTER;

        if (IsEqualIID(Interface, IID_IUnknown) ||
            IsEqualIID(Interface, IID_IVmGuestMemoryAccess))
        {
            *Object = static_cast<IVmGuestMemoryAccess *>(this);
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

    /*
     * The window is put where it was asked for, over whatever was there. The
     * memory the machine was built with is taken out from under it first,
     * because two things cannot answer for one page and the one that was there
     * is the ordinary memory nothing is using.
     */
    /*
     * Memory for a device to keep its own contents in.
     *
     * What is handed back stands for that memory rather than being it. The
     * device puts it somewhere the guest can see through the thing itself, so
     * nothing is mapped here: this only has to exist and answer.
     */
    STDMETHODIMP CreateDeviceMemoryBlock(ULONG64 Pages, ULONG Kind,
                                         ULONG Flags,
                                         IVmMemoryBlock **Block) override
    {
        if (!RepositoryQuiet)
        {
            printf("    a block of %llu page(s), kind %lu, flags %lu\n",
                   (unsigned long long)Pages, Kind, Flags);
        }

        if (Block == nullptr)
            return E_POINTER;

        *Block = MakeBlock(Pages * VDEV_PAGE_SIZE);

        return (*Block != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP CreateRamGpaRange() override { return E_NOTIMPL; }
    STDMETHODIMP CreateRamApertureFromByteRange() override { return E_NOTIMPL; }
    STDMETHODIMP CreateSectionBackedGpaRange() override { return E_NOTIMPL; }
    STDMETHODIMP CreateDaxFileBackedGpaRange() override { return E_NOTIMPL; }
    STDMETHODIMP RegisterForVtl2Access() override { return E_NOTIMPL; }

    STDMETHODIMP ReadRamBytes(ULONG64 Address, void *Buffer,
                              ULONG Length) override
    {
        const void *From = hv::Guest(Address, Length);

        if ((Buffer == nullptr) || (From == nullptr))
            return E_INVALIDARG;

        memcpy(Buffer, From, Length);
        return S_OK;
    }

    STDMETHODIMP WriteRamBytes(ULONG64 Address, const void *Buffer,
                               ULONG Length) override
    {
        void *To = hv::Guest(Address, Length);

        if ((Buffer == nullptr) || (To == nullptr))
            return E_INVALIDARG;

        memcpy(To, Buffer, Length);
        return S_OK;
    }

    STDMETHODIMP ReadRamBytesEx() override { return E_NOTIMPL; }
    STDMETHODIMP WriteRamBytesEx() override { return E_NOTIMPL; }
    STDMETHODIMP CreateNotificationWithHandler() override { return E_NOTIMPL; }
    STDMETHODIMP GetHclErrorPageLocations() override { return E_NOTIMPL; }

    /*
     * Where a guest address really is. Asked of the processor rather than
     * worked out here, because what the answer is depends on the page tables
     * the guest has up at this moment and nothing else knows them.
     */
    STDMETHODIMP TranslateGvaToGpa(ULONG64 Address,
                                   ULONG64 *Physical) override
    {
        if (Physical == nullptr)
            return E_POINTER;

        return hv::Reachable(Address, Physical) ? S_OK : E_FAIL;
    }

    STDMETHODIMP CreateMemoryBlockPageAperture() override { return E_NOTIMPL; }
    STDMETHODIMP DestroyAperture() override { return E_NOTIMPL; }
    STDMETHODIMP RegisterForEmulationOnMemoryWrite() override { return E_NOTIMPL; }
    STDMETHODIMP UnregisterEmulationOnMemoryWrite() override { return E_NOTIMPL; }

private:
    volatile LONG m_Count = 1;
};
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

            if (IsEqualIID(Service, IID_IVmTimeSource))
            {
                printf(" (given)\n");
                m_Clock.AddRef();
                *Object = static_cast<IVmTimeSource *>(&m_Clock);
                return S_OK;
            }

            if (IsEqualIID(Service, IID_IMonitorDevice) &&
                (m_Screen != nullptr))
            {
                printf(" (given)\n");
                m_Screen->AddRef();
                *Object = m_Screen;
                return S_OK;
            }

            if (IsEqualIID(Service, IID_IVmBios))
            {
                printf(" (given)\n");
                m_Bios.AddRef();
                *Object = static_cast<IVmBios *>(&m_Bios);
                return S_OK;
            }

            if (IsEqualIID(Service, IID_IVmGuestMemoryAccess))
            {
                printf(" (given)\n");
                m_Memory.AddRef();
                *Object = static_cast<IVmGuestMemoryAccess *>(&m_Memory);
                return S_OK;
            }

            /*
             * And then whatever is already in the machine. A service that one
             * device offers and another wants does not come from the host at
             * all: the host only knows who to ask, and asking is done by
             * offering each of them the identifier and seeing who answers.
             */
            for (ULONG Index = 0; Index < m_Offered; Index++)
            {
                if (SUCCEEDED(m_Device[Index]->QueryInterface(Service, Object)))
                {
                    printf(" (from another part)\n");
                    return S_OK;
                }
            }

            /* And a stand-in for the rest, unless it said it can do without */
            void *Instead = IsSpare(Service) ? nullptr : StandIn(Service);

            if (Instead != nullptr)
            {
                printf(" (stood in for)\n");
                *Object = Instead;
                return S_OK;
            }
        }

        printf("\n");
        return E_NOINTERFACE;
    }

    /* Something already in the machine, which the next one in may want */
    void Offer(IVirtualDevice *One) noexcept
    {
        if (m_Offered < ARRAYSIZE(m_Device))
            m_Device[m_Offered++] = One;
    }

    /* And whatever is looking at the machine, for a display device to tell */
    void Watch(IMonitorDevice *Screen) noexcept { m_Screen = Screen; }

    /*
     * One this device said it would come up without.
     *
     * Those are never stood in for. A stand-in is there to get a device past
     * something it cannot do without, and it lies about everything it is asked;
     * a device that was told it has something optional goes and uses it, and
     * what it gets back is whatever was on the stack. Refusing is both honest
     * and what the device is already written to survive.
     */
    void Spare(REFIID Service) noexcept
    {
        if (m_Spares < ARRAYSIZE(m_Spare))
            m_Spare[m_Spares++] = Service;
    }

    void Forget() noexcept { m_Spares = 0; }

    bool IsSpare(REFIID Service) const noexcept
    {
        for (ULONG Index = 0; Index < m_Spares; Index++)
        {
            if (IsEqualGUID(Service, m_Spare[Index]))
                return true;
        }

        return false;
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
    Clock m_Clock;
    Bios m_Bios;
    GuestMemory m_Memory;

    IVirtualDevice *m_Device[MACHINE_PARTS] = {};
    ULONG m_Offered = 0;
    IMonitorDevice *m_Screen = nullptr;

    GUID m_Spare[8] = {};
    ULONG m_Spares = 0;
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

/* Where a disk controller of this kind answers, and what it is asked */
#define IDE_DATA        0x01F0
#define IDE_LBA_MID     0x01F4
#define IDE_LBA_HIGH    0x01F5
#define IDE_SELECT      0x01F6
#define IDE_COMMAND     0x01F7
#define IDE_IDENTIFY    0xEC

/* What a drive that is not an ordinary one says when asked the ordinary way */
#define IDE_PACKET_MID  0x14
#define IDE_PACKET_HIGH 0xEB

/*
 * The one thing a firmware asks a disk controller before anything else.
 *
 * A drive that takes packets refuses to say what it is the ordinary way, and
 * leaves two particular bytes in the address registers instead. Every firmware
 * there has ever been looks for exactly those two bytes, so a controller that
 * hands them back has a disc in it as far as anything is concerned.
 */
static void DriveOneChannel(const Emulation &Addresses, USHORT Base,
                            UCHAR Which)
{
    IVndIoPortHandler *Select = Addresses.Claimed(Base + 6);
    IVndIoPortHandler *Command = Addresses.Claimed(Base + 7);
    IVndIoPortHandler *Mid = Addresses.Claimed(Base + 4);
    IVndIoPortHandler *High = Addresses.Claimed(Base + 5);

    if ((Select == nullptr) || (Command == nullptr) || (Mid == nullptr) ||
        (High == nullptr))
    {
        printf("  %04x is not answered for at all\n", Base);
        return;
    }

    ULONG Status = 0;
    ULONG Low = 0;
    ULONG Top = 0;

    Select->NotifyIoPortWrite((USHORT)(Base + 6), 1, Which);
    Command->NotifyIoPortWrite((USHORT)(Base + 7), 1, IDE_IDENTIFY);
    Command->NotifyIoPortRead((USHORT)(Base + 7), 1, &Status);
    Mid->NotifyIoPortRead((USHORT)(Base + 4), 1, &Low);
    High->NotifyIoPortRead((USHORT)(Base + 5), 1, &Top);

    const bool Disc = (((Low & 0xFF) == IDE_PACKET_MID) &&
                       ((Top & 0xFF) == IDE_PACKET_HIGH));

    printf("  %04x %s: status %02lx, left %02lx %02lx%s\n", Base,
           (Which == 0xA0) ? "master" : "slave ",
           Status & 0xFF, Low & 0xFF, Top & 0xFF,
           Disc ? "   a disc" : "");
}

static void DriveIde(const Emulation &Addresses)
{
    printf("what is in each of the four places a drive can be:\n");

    DriveOneChannel(Addresses, 0x01F0, 0xA0);
    DriveOneChannel(Addresses, 0x01F0, 0xB0);
    DriveOneChannel(Addresses, 0x0170, 0xA0);
    DriveOneChannel(Addresses, 0x0170, 0xB0);
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

    if (IsEqualGUID(Which, CLSID_IdeControllerDevice))
    {
        DriveIde(Addresses);
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

bool ReadGuid(const char *Text, GUID &Which)
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


/* A MACHINE WITH REAL HARDWARE IN IT *****************************************/

/* Where a firmware of the older sort lives, and how a processor reaches it */
#define BIOS_BASE       0x000F0000ull
#define BIOS_SEGMENT    0xF000
#define BIOS_ENTRY      0xFFF0

/*
 * One device, brought all the way up.
 *
 * The library is loaded, asked for the kind, and the thing that comes back is
 * driven through the whole of the contract. Whatever it reserves lands in the
 * addresses the machine will dispatch from, because it is handed the same one
 * every other device here is.
 */
static IVirtualDevice *Fit(const Part &What, Probe &Handed)
{
    GUID Wanted = {};

    if (!ReadGuid(What.Class, Wanted))
    {
        printf("%s is not written the way a kind is named\n", What.Class);
        return nullptr;
    }

    const HMODULE Loaded = LoadLibraryA(What.Library);

    if (Loaded == nullptr)
    {
        printf("%s would not load, %lu\n", What.Library, GetLastError());
        return nullptr;
    }

    const auto Ask = reinterpret_cast<PFN_GET_CLASS_OBJECT>(
        GetProcAddress(Loaded, "DllGetClassObject"));

    if (Ask == nullptr)
    {
        printf("%s is not a class server\n", What.Library);
        return nullptr;
    }

    IClassFactory *Factory = nullptr;
    HRESULT Status = Ask(Wanted, IID_IClassFactory,
                         reinterpret_cast<void **>(&Factory));

    if (FAILED(Status))
    {
        printf("it has no such kind, %08lx\n", Status);
        return nullptr;
    }

    IVirtualDevice *Device = nullptr;

    Status = Factory->CreateInstance(nullptr, IID_IVirtualDevice,
                                     reinterpret_cast<void **>(&Device));
    Factory->Release();

    if (FAILED(Status))
    {
        printf("it would not make one, %08lx\n", Status);
        return nullptr;
    }

    /*
     * Which of what it wants it can do without, asked before it is handed any
     * of them. The answer is ordered, with those last, so where the break is
     * says which ones are not worth standing in for.
     */
    ULONG Count = 0;
    ULONG Required = 0;
    GUID *Services = nullptr;

    Handed.Forget();

    if (SUCCEEDED(Device->GetDependencies(nullptr, &Count, &Services,
                                          &Required)) &&
        (Services != nullptr))
    {
        for (ULONG Index = Required; Index < Count; Index++)
            Handed.Spare(Services[Index]);

        CoTaskMemFree(Services);
    }

    Status = Device->Initialize(&TheRepository, 0,
                                static_cast<IUnknown *>(&Handed));

    if (FAILED(Status))
    {
        printf("  it was refused something it must have, %08lx\n", Status);
        Device->Teardown();
        Device->Release();
        return nullptr;
    }

    /*
     * Whatever the kind takes for itself, before it is switched on. Which disc
     * is in a drive is that kind's own business and reaches it this way rather
     * than through anything the machine is told.
     */
    if (What.Settings != nullptr)
    {
        IRtvmDeviceSettings *Told = nullptr;

        if (SUCCEEDED(Device->QueryInterface(
                IID_IRtvmDeviceSettings, reinterpret_cast<void **>(&Told))))
        {
            const HRESULT Took = Told->SetSettings(What.Settings);

            printf("  it was told %s, %08lx\n", What.Settings, Took);
            Told->Release();
        }
        else
        {
            printf("  it takes nothing of its own, so %s went nowhere\n",
                   What.Settings);
        }
    }

    Device->StartReservingResources(&TheRepository, VDEV_STATE_NONE);
    Device->FinishReservingResources(VDEV_STATE_NONE);

    /*
     * Guarded, because this is somebody else's code being driven through a
     * contract that was worked out by reading. One kind that goes wrong should
     * cost the machine that kind and nothing else: a window that dies outright
     * says only that something was wrong, where a machine that comes up without
     * one part says which.
     */
    __try
    {
        Status = Device->PowerOnCold(VDEV_STATE_NONE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("  it came apart on the way up, %08lx\n",
               (ULONG)GetExceptionCode());
        return nullptr;
    }

    if (FAILED(Status))
    {
        printf("  it would not come on, %08lx\n", Status);
        Device->PowerOff(VDEV_STATE_NONE);
        Device->Teardown();
        Device->Release();
        return nullptr;
    }

    return Device;
}

/* What the guest asked for that nothing in the machine answers for */
struct Unclaimed
{
    USHORT Port;
    ULONG Times;
};

static Unclaimed TheUnclaimed[64];
static ULONG TheUnclaimedCount = 0;

static void NoteUnclaimed(USHORT Port)
{
    for (ULONG Index = 0; Index < TheUnclaimedCount; Index++)
    {
        if (TheUnclaimed[Index].Port == Port)
        {
            TheUnclaimed[Index].Times++;
            return;
        }
    }

    if (TheUnclaimedCount >= ARRAYSIZE(TheUnclaimed))
        return;

    TheUnclaimed[TheUnclaimedCount].Port = Port;
    TheUnclaimed[TheUnclaimedCount].Times = 1;
    TheUnclaimedCount++;
}

/*
 * A guest access to a port, given to whichever device reserved it.
 *
 * What a port that nobody reserved reads as is not nothing: a board answers a
 * floating bus with every bit set, and a firmware that looks for hardware by
 * reading a port and checking for zero finds hardware everywhere if it is told
 * zero instead.
 */
static ULONG64 TheAnswered = 0;

/*
 * A run of ports to write down every access to.
 *
 * A firmware and a device that disagree do it one port at a time, and the whole
 * of the disagreement is in the order and the values. Nothing else here shows
 * that, because everything else counts accesses rather than reading them.
 */
static ULONG TheWatchFirst = 1;
static ULONG TheWatchLast = 0;
static ULONG TheWatched = 0;

void VmWatch(ULONG First, ULONG Last)
{
    TheWatchFirst = First;
    TheWatchLast = Last;
    TheWatched = 0;
}

/* How many are worth seeing before it is the same thing over and over */
#define WATCH_ENOUGH 400

static void Watch(USHORT Port, USHORT Width, ULONG Value, bool Writing,
                  bool Answered)
{
    if ((Port < TheWatchFirst) || (Port > TheWatchLast))
        return;

    if (TheWatched++ >= WATCH_ENOUGH)
        return;

    const ULONG Mask = (Width == 1) ? 0xFFu : ((Width == 2) ? 0xFFFFu : ~0u);

    printf("  %04x %s %0*lx%s\n", Port, Writing ? "<-" : "->",
           (Width == 1) ? 2 : ((Width == 2) ? 4 : 8), Value & Mask,
           Answered ? "" : "  (nothing answers for it)");
}

/*
 * What a firmware said on its way through.
 *
 * The first serial port is where anything of this age says what it is doing,
 * and there is no device here to take it. Rather than count the writes and
 * throw the bytes away, they are kept: a firmware's own account of its boot is
 * worth more than the number of times it wrote one.
 */
#define SERIAL_PORT     0x03F8
#define SERIAL_SAID     4096

static char TheSaid[SERIAL_SAID];
static ULONG TheSaidCount = 0;

static void Overheard(UCHAR One)
{
    if (TheSaidCount < (SERIAL_SAID - 1))
        TheSaid[TheSaidCount++] = (char)One;
}

/* What each way of stopping is called, for the ones worth naming */
static const char *WhyStopped(ULONG Reason)
{
    switch (Reason)
    {
        case WHvRunVpExitReasonNone: return "nothing";
        case WHvRunVpExitReasonMemoryAccess: return "a place in memory";
        case WHvRunVpExitReasonX64IoPortAccess: return "a port";
        case WHvRunVpExitReasonUnrecoverableException: return "a fault it cannot come back from";
        case WHvRunVpExitReasonInvalidVpRegisterValue: return "a register it cannot hold";
        case WHvRunVpExitReasonUnsupportedFeature: return "something this processor has not got";
        case WHvRunVpExitReasonX64InterruptWindow: return "being able to take an interrupt";
        case WHvRunVpExitReasonX64Halt: return "having stopped on purpose";
        case WHvRunVpExitReasonX64ApicEoi: return "an interrupt being finished";
        case WHvRunVpExitReasonX64MsrAccess: return "a machine specific register";
        case WHvRunVpExitReasonX64Cpuid: return "asking what it is";
        case WHvRunVpExitReasonException: return "a fault";
        default: return "something unnamed";
    }
}

/*
 * A whole run of accesses to one port, carried out.
 *
 * A firmware moves a sector this way rather than one word at a time, and the
 * processor stops once for the whole run rather than once for each. Doing one
 * and calling it the run leaves the guest's pointer and count wrong, so all of
 * it is done here and the registers are put where the instruction would have
 * left them.
 *
 * Which way the pointer moves is the direction flag, which is the one thing
 * about the instruction that is not in what the processor handed over.
 */
static bool Run(const WHV_RUN_VP_EXIT_CONTEXT &Exit, const Emulation &On)
{
    const USHORT Port = Exit.IoPortAccess.PortNumber;
    const USHORT Width = (USHORT)Exit.IoPortAccess.AccessInfo.AccessSize;
    const bool Writing = (Exit.IoPortAccess.AccessInfo.IsWrite != 0);
    IVndIoPortHandler *Handler = On.Claimed(Port);

    ULONG64 Count = 1;

    if (Exit.IoPortAccess.AccessInfo.RepPrefix)
        Count = Exit.IoPortAccess.Rcx;

    /* Backwards when the guest said so, which is rare but is allowed */
    const bool Backwards = ((Exit.VpContext.Rflags & 0x400) != 0);
    const LONG64 Step = Backwards ? -(LONG64)Width : (LONG64)Width;

    ULONG64 From = Exit.IoPortAccess.Ds.Base + Exit.IoPortAccess.Rsi;
    ULONG64 To = Exit.IoPortAccess.Es.Base + Exit.IoPortAccess.Rdi;

    for (ULONG64 Index = 0; Index < Count; Index++)
    {
        if (Writing)
        {
            const void *Where = hv::Guest(From, Width);
            ULONG Value = 0;

            if (Where == nullptr)
                return false;

            memcpy(&Value, Where, Width);

            if (Handler != nullptr)
            {
                Handler->NotifyIoPortWrite(Port, Width, Value);
                TheAnswered++;
            }
            else
            {
                NoteUnclaimed(Port);
            }

            From += Step;
        }
        else
        {
            void *Where = hv::Guest(To, Width);
            ULONG Value = 0xFFFFFFFF;

            if (Where == nullptr)
                return false;

            if (Handler != nullptr)
            {
                if (FAILED(Handler->NotifyIoPortRead(Port, Width, &Value)))
                    Value = 0xFFFFFFFF;

                TheAnswered++;
            }
            else
            {
                NoteUnclaimed(Port);
            }

            memcpy(Where, &Value, Width);
            To += Step;
        }
    }

    /* Where the instruction would have left them, had it been carried out */
    if (Writing)
    {
        hv::Poke(WHvX64RegisterRsi,
                 Exit.IoPortAccess.Rsi + (ULONG64)(Step * (LONG64)Count));
    }
    else
    {
        hv::Poke(WHvX64RegisterRdi,
                 Exit.IoPortAccess.Rdi + (ULONG64)(Step * (LONG64)Count));
    }

    if (Exit.IoPortAccess.AccessInfo.RepPrefix)
        hv::Poke(WHvX64RegisterRcx, 0);

    return true;
}

static void Dispatch(const WHV_RUN_VP_EXIT_CONTEXT &Exit, const Emulation &On)
{
    const USHORT Port = Exit.IoPortAccess.PortNumber;
    const USHORT Width = (USHORT)Exit.IoPortAccess.AccessInfo.AccessSize;
    IVndIoPortHandler *Handler = On.Claimed(Port);

    if (Exit.IoPortAccess.AccessInfo.IsWrite)
    {
        ULONG Value = (ULONG)Exit.IoPortAccess.Rax;

        if (Width == 1)
            Value &= 0xFF;
        else if (Width == 2)
            Value &= 0xFFFF;

        Watch(Port, Width, Value, true, Handler != nullptr);

        if (Handler != nullptr)
        {
            Handler->NotifyIoPortWrite(Port, Width, Value);
            TheAnswered++;
        }
        else
        {
            NoteUnclaimed(Port);

            if (Port == SERIAL_PORT)
                Overheard((UCHAR)Value);
        }

        return;
    }

    ULONG Value = 0xFFFFFFFF;

    if (Handler != nullptr)
    {
        if (FAILED(Handler->NotifyIoPortRead(Port, Width, &Value)))
            Value = 0xFFFFFFFF;

        TheAnswered++;
    }
    else
    {
        NoteUnclaimed(Port);
    }

    Watch(Port, Width, Value, false, Handler != nullptr);

    /* Only as much of the register as was asked for, the rest left alone */
    ULONG64 Rax = Exit.IoPortAccess.Rax;

    if (Width == 1)
        Rax = (Rax & ~0xFFull) | (Value & 0xFF);
    else if (Width == 2)
        Rax = (Rax & ~0xFFFFull) | (Value & 0xFFFF);
    else
        Rax = Value;

    hv::Poke(WHvX64RegisterRax, Rax);
}

int Assemble(const char *Bios, const Part *Parts, ULONG Many,
                    ULONG64 Ram, ULONG Steps)
{
    if (!hv::Open(Ram))
    {
        printf("a machine could not be made\n");
        return 1;
    }

    printf("%llu MB of memory\n", (unsigned long long)(Ram / (1024 * 1024)));

    /*
     * The hardware first, because a firmware that starts before the devices it
     * talks to have reserved anything reads a machine with nothing in it and
     * decides there is nothing in it.
     */
    /* With somewhere to draw, because a display device will not come up without */
    Probe Handed;
    Monitor Screen;

    Handed.Watch(&Screen);
    IVirtualDevice *Fitted[MACHINE_PARTS] = {};
    ULONG Fittings = 0;

    for (ULONG Index = 0; (Index < Many) && (Index < MACHINE_PARTS); Index++)
    {
        printf("fitting %s:\n", Parts[Index].Class);

        RepositoryQuiet = true;
        TheFitting = Parts[Index].Class;
        TheFittingSettings = Parts[Index].Settings;

        IVirtualDevice *One = Fit(Parts[Index], Handed);

        if (One != nullptr)
        {
            Fitted[Fittings++] = One;
            Handed.Offer(One);
            printf("  it is in\n");
        }
    }

    printf("%lu of %lu part(s) are in, answering for %lu run(s) of ports\n",
           Fittings, Many, Handed.Addresses().Ports());

    /* And then the firmware, read in and put where one of its age is found */
    const HANDLE File = CreateFileA(Bios, GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);

    if (File == INVALID_HANDLE_VALUE)
    {
        printf("%s would not open, %lu\n", Bios, GetLastError());
        return 1;
    }

    LARGE_INTEGER Size = {};

    GetFileSizeEx(File, &Size);

    auto *Image = static_cast<UCHAR *>(malloc((size_t)Size.QuadPart));
    DWORD Read = 0;

    if ((Image == nullptr) ||
        !ReadFile(File, Image, (DWORD)Size.QuadPart, &Read, nullptr))
    {
        printf("%s would not read\n", Bios);
        CloseHandle(File);
        return 1;
    }

    CloseHandle(File);

    /*
     * At the top of the first megabyte, and the last of it has to be the last
     * of the firmware: a processor coming out of reset reads the sixteen bytes
     * below the end of that segment and does whatever it finds there.
     */
    const ULONG64 Where = 0x00100000ull - Read;

    if (!hv::Place(Image, Read, Where))
    {
        printf("the firmware would not go at %012llx\n",
               (unsigned long long)Where);
        return 1;
    }

    printf("firmware %s, %lu bytes at %05llx\n", Bios, Read,
           (unsigned long long)Where);

    if (!hv::Start(BIOS_SEGMENT, BIOS_BASE, BIOS_ENTRY))
    {
        printf("it would not be started\n");
        return 1;
    }

    printf("running from %04x:%04x\n\n", BIOS_SEGMENT, (unsigned)BIOS_ENTRY);

    WHV_RUN_VP_EXIT_CONTEXT Exit = {};
    ULONG64 Count = 0;
    ULONG64 Ports = 0;
    ULONG64 Rings = 0;

    while (Count < Steps)
    {
        if (!hv::Step(&Exit))
        {
            printf("the processor would not run\n");
            break;
        }

        Count++;

        if (Exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess)
        {
            /* A whole run at once, which is how a sector is moved */
            if (Exit.IoPortAccess.AccessInfo.StringOp ||
                Exit.IoPortAccess.AccessInfo.RepPrefix)
            {
                if (!Run(Exit, Handed.Addresses()))
                {
                    printf("a run of accesses to %04x went outside the memory "
                           "it has\n", Exit.IoPortAccess.PortNumber);
                    break;
                }
            }
            else
            {
                Dispatch(Exit, Handed.Addresses());
            }

            Ports++;
        }
        else if (Exit.ExitReason != WHvRunVpExitReasonMemoryAccess)
        {
            printf("%6llu  %04x:%04llx  stopped for %s\n",
                   (unsigned long long)Count, Exit.VpContext.Cs.Selector,
                   (unsigned long long)Exit.VpContext.Rip,
                   WhyStopped(Exit.ExitReason));

            if (Exit.ExitReason != WHvRunVpExitReasonX64Cpuid)
                break;
        }

        if (Exit.VpContext.InstructionLength == 0)
        {
            printf("%6llu  %04x:%04llx  nothing to go past\n",
                   (unsigned long long)Count, Exit.VpContext.Cs.Selector,
                   (unsigned long long)Exit.VpContext.Rip);
            break;
        }

        hv::Poke(WHvX64RegisterRip,
                 Exit.VpContext.Rip + Exit.VpContext.InstructionLength);

        /*
         * And anything that was due while the processor was running. Between
         * one stop and the next is the only safe moment for it: a device being
         * called back in the middle of answering a port would be re-entered.
         */
        Rings += Ring();
    }

    printf("\nstopped after %llu, at %04x:%04llx\n",
           (unsigned long long)Count, Exit.VpContext.Cs.Selector,
           (unsigned long long)Exit.VpContext.Rip);
    printf("  %llu of those were ports, and the hardware answered for %llu\n",
           (unsigned long long)Ports, (unsigned long long)TheAnswered);

    for (ULONG Index = 0; Index < TheTimerCount; Index++)
    {
        printf("  %s set a timer %lu time(s), and it went off %lu\n",
               TheTimers[Index]->Whose(), TheTimers[Index]->Arms(),
               TheTimers[Index]->Rings());
    }

    /* And whatever it said on the way, which nothing here was listening to */
    if (TheSaidCount != 0)
    {
        TheSaid[TheSaidCount] = '\0';
        printf("\nwhat the firmware said:\n%s\n", TheSaid);
    }

    if (Rings != 0)
        printf("  timers went off %llu time(s)\n", (unsigned long long)Rings);

    if (Handed.Wires().Raised() != 0)
        printf("  a line was raised %lu time(s)\n", Handed.Wires().Raised());

    if (TheUnclaimedCount != 0)
    {
        printf("  and it reached for %lu port(s) nothing here answers for:\n",
               TheUnclaimedCount);

        for (ULONG Index = 0; Index < TheUnclaimedCount; Index++)
        {
            printf("    %04x, %lu time(s)\n", TheUnclaimed[Index].Port,
                   TheUnclaimed[Index].Times);
        }
    }

    hv::Tell();

    for (ULONG Index = 0; Index < Fittings; Index++)
    {
        Fitted[Index]->PowerOff(VDEV_STATE_NONE);
        Fitted[Index]->Teardown();
        Fitted[Index]->Release();
    }

    hv::Close();
    return 0;
}


/* A MACHINE A FRONT END KEEPS ************************************************/

/*
 * One of these, because there is one processor and one screen. What it holds is
 * everything that has to outlive a slice of running: the parts, what they were
 * handed, and where the processor had got to when it was last let go of.
 */
static struct
{
    Probe *Handed;
    Monitor *Screen;

    IVirtualDevice *Fitted[MACHINE_PARTS];
    ULONG Fittings;

    IRtvmTextSurface *Text;
    IRtvmPixelSurface *Pixels;

    WHV_RUN_VP_EXIT_CONTEXT Exit;
    ULONG64 Count;
    ULONG64 Ports;

    bool Open;
    bool Stopped;

    VmWanted Wanted;
} TheVm;

void VmAllowStandIns(bool Allowed) { StrangersAllowed = Allowed; }
void VmStoreAnswers(HRESULT What) { RepositoryAnswer = What; }
void VmQuiet(bool Quiet) { RepositoryQuiet = Quiet; }

const char *VmSaid()
{
    const ULONG Where = (TheSaidCount < (SERIAL_SAID - 1)) ? TheSaidCount
                                                           : (SERIAL_SAID - 1);

    TheSaid[Where] = '\0';
    return TheSaid;
}

const void *VmGuest(ULONG64 Where, ULONG Length)
{
    return hv::Guest(Where, Length);
}

/*
 * A key going down or coming up, handed to whichever part is a keyboard.
 *
 * Which part that is is not known in advance and is not worth remembering: a
 * machine has one keyboard and finding it is asking each part whether it is
 * one, which is the same question the parts ask each other.
 */
bool VmKey(USHORT Code, bool Down, bool Extended)
{
    bool Went = false;

    for (ULONG Index = 0; Index < TheVm.Fittings; Index++)
    {
        IVmKeyboardDevice *Keyboard = nullptr;

        if (FAILED(TheVm.Fitted[Index]->QueryInterface(
                IID_IVmKeyboardDevice,
                reinterpret_cast<void **>(&Keyboard))))
            continue;

        VDEV_KEYSTROKE Key = {};

        Key.Code = Code;
        Key.Flags = (USHORT)((Down ? VDEV_KEY_DOWN : VDEV_KEY_UP) |
                             (Extended ? VDEV_KEY_EXTENDED : 0));

        Went = SUCCEEDED(Keyboard->SendKeystroke(&Key));
        Keyboard->Release();

        if (Went)
            break;
    }

    return Went;
}

IRtvmTextSurface *VmText() { return TheVm.Text; }
IRtvmPixelSurface *VmPixels() { return TheVm.Pixels; }

bool VmDirty(VDEV_VIDEO_KIND *Which)
{
    if (TheVm.Screen == nullptr)
        return false;

    return TheVm.Screen->Take(Which);
}

bool VmStopped() { return TheVm.Stopped; }
ULONG64 VmCount() { return TheVm.Count; }

ULONG64 VmWhere()
{
    return ((ULONG64)TheVm.Exit.VpContext.Cs.Selector << 16) |
           (TheVm.Exit.VpContext.Rip & 0xFFFF);
}

/* Read in and put where one of its age lives, which is the last of the megabyte */
static bool Load(const char *Bios)
{
    const HANDLE File = CreateFileA(Bios, GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);

    if (File == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER Size = {};

    GetFileSizeEx(File, &Size);

    auto *Image = static_cast<UCHAR *>(malloc((size_t)Size.QuadPart));
    DWORD Read = 0;

    if ((Image == nullptr) ||
        !ReadFile(File, Image, (DWORD)Size.QuadPart, &Read, nullptr))
    {
        free(Image);
        CloseHandle(File);
        return false;
    }

    CloseHandle(File);

    const bool Went = hv::Place(Image, Read, 0x00100000ull - Read);

    free(Image);
    return Went;
}

bool VmOpen(const VmWanted &What)
{
    VmClose();

    TheVm.Handed = new Probe;
    TheVm.Screen = new Monitor;

    if ((TheVm.Handed == nullptr) || (TheVm.Screen == nullptr))
        return false;

    TheVm.Wanted = What;

    TheSaidCount = 0;
    TheUnclaimedCount = 0;
    TheAnswered = 0;

    if (!hv::Open((What.Ram != 0) ? What.Ram : 0x08000000ull))
        return false;

    TheVm.Open = true;

    /* Offered first, so that a display device asking for one is given it */
    TheVm.Handed->Watch(TheVm.Screen);

    for (ULONG Index = 0; (Index < What.Many) && (Index < MACHINE_PARTS);
         Index++)
    {
        TheFitting = What.Parts[Index].Class;
        TheFittingSettings = What.Parts[Index].Settings;

        IVirtualDevice *One = Fit(What.Parts[Index], *TheVm.Handed);

        if (One == nullptr)
            continue;

        TheVm.Fitted[TheVm.Fittings++] = One;
        TheVm.Handed->Offer(One);

        /* And whether it is something there is anything to look at on */
        if (TheVm.Text == nullptr)
        {
            One->QueryInterface(IID_IRtvmTextSurface,
                                reinterpret_cast<void **>(&TheVm.Text));
        }

        if (TheVm.Pixels == nullptr)
        {
            One->QueryInterface(IID_IRtvmPixelSurface,
                                reinterpret_cast<void **>(&TheVm.Pixels));
        }
    }

    if (!Load(What.Bios))
        return false;

    memset(&TheVm.Exit, 0, sizeof(TheVm.Exit));
    return hv::Start(BIOS_SEGMENT, BIOS_BASE, BIOS_ENTRY);
}

ULONG VmRun(ULONG Steps)
{
    ULONG Done = 0;

    if (!TheVm.Open || TheVm.Stopped)
        return 0;

    while (Done < Steps)
    {
        if (!hv::Step(&TheVm.Exit))
        {
            TheVm.Stopped = true;
            break;
        }

        Done++;
        TheVm.Count++;

        if (TheVm.Exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess)
        {
            if (TheVm.Exit.IoPortAccess.AccessInfo.StringOp ||
                TheVm.Exit.IoPortAccess.AccessInfo.RepPrefix)
            {
                if (!Run(TheVm.Exit, TheVm.Handed->Addresses()))
                {
                    TheVm.Stopped = true;
                    break;
                }
            }
            else
            {
                Dispatch(TheVm.Exit, TheVm.Handed->Addresses());
            }
            TheVm.Ports++;
        }
        else if ((TheVm.Exit.ExitReason != WHvRunVpExitReasonMemoryAccess) &&
                 (TheVm.Exit.ExitReason != WHvRunVpExitReasonX64Cpuid))
        {
            TheVm.Stopped = true;
            break;
        }

        if (TheVm.Exit.VpContext.InstructionLength == 0)
        {
            TheVm.Stopped = true;
            break;
        }

        hv::Poke(WHvX64RegisterRip,
                 TheVm.Exit.VpContext.Rip +
                 TheVm.Exit.VpContext.InstructionLength);

        Ring();
    }

    return Done;
}

/*
 * Reset by building again rather than by putting the registers back. The parts
 * are what have to be told, not the processor: one that has been running holds
 * state the firmware will not set up a second time.
 */
bool VmReset()
{
    const VmWanted Again = TheVm.Wanted;

    VmClose();
    return VmOpen(Again);
}

void VmClose()
{
    if (TheVm.Text != nullptr)
    {
        TheVm.Text->Release();
        TheVm.Text = nullptr;
    }

    if (TheVm.Pixels != nullptr)
    {
        TheVm.Pixels->Release();
        TheVm.Pixels = nullptr;
    }

    /* Backwards, so that a part goes after whatever was leaning on it */
    while (TheVm.Fittings > 0)
    {
        IVirtualDevice *One = TheVm.Fitted[--TheVm.Fittings];

        TheVm.Fitted[TheVm.Fittings] = nullptr;

        if (One == nullptr)
            continue;

        One->PowerOff(VDEV_STATE_NONE);
        One->Teardown();
        One->Release();
    }

    for (ULONG Index = 0; Index < TheTimerCount; Index++)
        TheTimers[Index]->Release();

    TheTimerCount = 0;
    TheStrangerCount = 0;

    if (TheVm.Open)
    {
        hv::Close();
        TheVm.Open = false;
    }

    delete TheVm.Handed;
    TheVm.Handed = nullptr;

    delete TheVm.Screen;
    TheVm.Screen = nullptr;

    TheVm.Stopped = false;
    TheVm.Count = 0;
    TheVm.Ports = 0;
}

/* ONE KIND OF DEVICE, ON ITS OWN *********************************************/

/*
 * Loaded, asked for, driven, and taken apart, with nothing else in the machine.
 *
 * A machine is not built for this. What is being asked is whether the contract
 * is right, and the answer to that is in what the device reaches for on its way
 * up rather than in anything it does afterwards.
 */
int One(const char *Library, const char *Class, const char *Settings)
{
    /* The one being driven, so that it is told the configuration of its kind */
    TheFitting = Class;
    TheFittingSettings = Settings;

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
    /* With somewhere to draw, because a display device will not come up without */
    Probe Handed;
    Monitor Screen;

    Handed.Watch(&Screen);

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
    /*
     * Nothing further is asked of one that did not come up. A device refused
     * something it said it must have is part way through building itself, and
     * the next call in goes at whatever it had not got round to making yet.
     */
    if (FAILED(Came))
    {
        printf("it was refused something it must have, so it is not driven\n");
        Device->Teardown();
        Device->Release();
        return 0;
    }

    printf("asking where it answers:\n");
    Status = Device->StartReservingResources(&TheRepository, VDEV_STATE_NONE);
    printf("it started with %08lx\n", Status);

    if (SUCCEEDED(Status))
    {
        Status = Device->FinishReservingResources(VDEV_STATE_NONE);
        printf("and settled with %08lx\n", Status);
    }

    printf("switching it on:\n");
    Status = Device->PowerOnCold(VDEV_STATE_NONE);
    printf("it came on with %08lx, having asked for %lu run(s) of ports\n",
           Status, Handed.Addresses().Ports());

    /*
     * And then whether it answers, but only if it came on. One that did not has
     * given back everything it took on the way, so the ports it asked for are
     * answered for by something that is no longer there.
     */
    if (SUCCEEDED(Status))
        Drive(Handed, Wanted);
    else
        printf("so there is nothing left of it to drive\n");

    if (Handed.Wires().Raised() != 0)
        printf("and it raised its line %lu time(s)\n", Handed.Wires().Raised());

    Device->PowerOff(VDEV_STATE_NONE);
    Device->Teardown();
    Device->Release();
    return 0;
}
