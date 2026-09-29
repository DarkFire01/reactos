/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The contract between the manager and a virtual device
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A device library is a class server. It publishes a kind of device under a
 * class identifier, the manager asks for one, and what comes back is driven
 * through the interfaces below for the rest of its life.
 *
 * The identifiers and the order of the slots are not this project's to choose:
 * they are what a device library built for a machine of this kind already
 * speaks, and a manager that wants to load one has to speak the same. What each
 * of them does inside is another matter and is nobody's but ours.
 */

#pragma once

#include <windows.h>
#include <objbase.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ones named before they are laid out, because something wants them first */
struct IVideoVdev;
struct IVmMemoryBlock;
struct IVmMouseDevice;

/* THE DEVICES A LIBRARY OFFERS ***********************************************/

/* The interrupt controllers, the timer, the transfer controller and the rest */
DEFINE_GUID(CLSID_PicDevice,
            0x9edd1639, 0x9bca, 0x40dc, 0xb3, 0xa2, 0x07, 0xc8, 0x28, 0xda, 0x60, 0xb5);
DEFINE_GUID(CLSID_PitDevice,
            0xa28e4d02, 0x3323, 0x4148, 0x95, 0x69, 0x56, 0x59, 0x30, 0xa5, 0xcb, 0x39);
DEFINE_GUID(CLSID_DmaControllerDevice,
            0x87045ce9, 0x5323, 0x438f, 0x93, 0xbb, 0x1e, 0x83, 0xdc, 0xbc, 0xe1, 0x8e);
DEFINE_GUID(CLSID_KeyboardControllerDevice,
            0x655bc5c5, 0xa784, 0x46b7, 0x81, 0xbc, 0xe2, 0x63, 0x28, 0xf7, 0xeb, 0x0e);
DEFINE_GUID(CLSID_SpeakerDevice,
            0x4d46d139, 0x7821, 0x4dc4, 0x98, 0xa3, 0x01, 0xe9, 0x8a, 0x58, 0x6a, 0x44);
DEFINE_GUID(CLSID_SuperIoDevice,
            0x35b0b12f, 0xa0d7, 0x482f, 0x80, 0xa0, 0xf5, 0x2f, 0x1a, 0xb3, 0xda, 0x2e);
DEFINE_GUID(CLSID_IsaBusDevice,
            0x4d42d9f7, 0x6531, 0x4f6c, 0x9e, 0x46, 0x1f, 0x04, 0x77, 0x87, 0x61, 0x04);
DEFINE_GUID(CLSID_PciBusDevice,
            0x84535fad, 0x4d98, 0x4a6a, 0xbd, 0xcd, 0x21, 0xd5, 0x72, 0x0d, 0xc4, 0x30);
DEFINE_GUID(CLSID_VideoS3Device,
            0x7d80d3db, 0x61ee, 0x4879, 0x88, 0x79, 0x56, 0x09, 0xf1, 0x10, 0x0a, 0xd0);

/* The ones the emulated storage library has */
DEFINE_GUID(CLSID_IdeControllerDevice,
            0x83f8638b, 0x8dca, 0x4152, 0x9e, 0xda, 0x2c, 0xa8, 0xb3, 0x30, 0x39, 0xb4);
DEFINE_GUID(CLSID_FloppyControllerDevice,
            0x8f0d2762, 0x0b00, 0x4e04, 0xaf, 0x4f, 0x19, 0x01, 0x05, 0x27, 0xcb, 0x93);

/* And the ones the chipset library has */
DEFINE_GUID(CLSID_IoApicDevice,
            0x72682fc4, 0x040a, 0x430a, 0xbe, 0x0b, 0x22, 0x45, 0x74, 0xb9, 0x53, 0xfe);
DEFINE_GUID(CLSID_RealTimeClockDevice,
            0xe51b7ef6, 0x4a7f, 0x4780, 0xaa, 0xae, 0xd4, 0xb2, 0x91, 0xaa, 0xcd, 0x2e);
DEFINE_GUID(CLSID_BiosLoaderDevice,
            0xac6b8dc1, 0x3257, 0x4a70, 0xb1, 0xb2, 0xa9, 0xc9, 0x21, 0x56, 0x59, 0xad);
DEFINE_GUID(CLSID_PowerManagementDevice,
            0xdb8b9818, 0xb4bb, 0x4725, 0xb9, 0x9d, 0xb4, 0x61, 0x27, 0x16, 0xb6, 0xb4);
DEFINE_GUID(CLSID_BatteryDevice,
            0xd465d87d, 0x6339, 0x4ff4, 0x93, 0xd9, 0x73, 0x53, 0xe3, 0x88, 0xd8, 0x89);
DEFINE_GUID(CLSID_GuestEmulationDevice,
            0x455c0f1b, 0xd51b, 0x40b1, 0xbe, 0xac, 0x87, 0x37, 0x7f, 0xe6, 0xe0, 0x41);

/* WHAT EVERY DEVICE IS *******************************************************/

DEFINE_GUID(IID_IVirtualDevice,
            0x0693ed7d, 0x8a8a, 0x4d87, 0xa4, 0x68, 0x11, 0x03, 0xb8, 0xc6, 0x3d, 0x9c);

/*
 * Carried into every step of the lifecycle below. Most devices never read it;
 * the ones that do test single bits, so it is a mask and not a count.
 */
typedef ULONG VDEV_STATE;

/* Nothing special is being asked for, which is what a cold start passes */
#define VDEV_STATE_NONE 0x00000000

/* The step is being taken for a machine that saves and comes back */
#define VDEV_STATE_PERSISTENT 0x00000020

/*
 * How a device is brought up, run and put away. The manager holds one of these
 * for every device it has and drives nothing else through anything but this.
 *
 * The slots past Release are in the order a device library lays them out, which
 * is why the ones that are not yet understood are still here: leaving one out
 * would move every slot below it.
 */
#undef INTERFACE
#define INTERFACE IVirtualDevice
DECLARE_INTERFACE_(IVirtualDevice, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /*
     * Which services it wants, as a list of their identifiers.
     *
     * The list is ordered: the ones it cannot come up without come first and
     * the ones it will do without come last, and Required says where the break
     * between them is. It is a count and not a mask of which, so a device that
     * wants more than a mask would hold is still answered properly.
     */
    STDMETHOD(GetDependencies)(THIS_ _In_opt_ PVOID Repository,
                               _Out_ PULONG Count,
                               _Outptr_ GUID **Services,
                               _Out_ PULONG Required) PURE;

    /* Handed its configuration and something to reach those services through */
    STDMETHOD(Initialize)(THIS_ _In_opt_ PVOID Repository,
                          _In_ ULONG_PTR Reserved,
                          _In_ IUnknown *Provider) PURE;
    STDMETHOD(Teardown)(THIS) PURE;

    /* Where it asks for the ports, the memory and the lines it answers for */
    STDMETHOD(StartReservingResources)(THIS_ _In_opt_ PVOID Repository,
                                       _In_ VDEV_STATE State) PURE;
    STDMETHOD(FinishReservingResources)(THIS_ _In_ VDEV_STATE State) PURE;
    STDMETHOD(FreeReservedResources)(THIS) PURE;
    STDMETHOD(SaveReservedResources)(THIS_ _In_opt_ PVOID Repository) PURE;

    /* Coming up from nothing, or into a state that was written out before */
    STDMETHOD(PowerOnCold)(THIS_ _In_ VDEV_STATE State) PURE;
    STDMETHOD(PowerOnRestore)(THIS_ _In_opt_ PVOID Repository,
                              _In_ VDEV_STATE State) PURE;
    STDMETHOD(PowerOff)(THIS_ _In_ VDEV_STATE State) PURE;
    STDMETHOD(Save)(THIS_ _In_opt_ PVOID Repository,
                    _In_ VDEV_STATE State) PURE;
    STDMETHOD(Resume)(THIS_ _In_ VDEV_STATE State) PURE;
    STDMETHOD(Pause)(THIS_ _In_ VDEV_STATE State) PURE;

    STDMETHOD(EnableOptimizations)(THIS_ _In_ VDEV_STATE State) PURE;
    STDMETHOD(StartDisableOptimizations)(THIS_ _In_ VDEV_STATE State) PURE;
    STDMETHOD(FinishDisableOptimizations)(THIS_ _In_ VDEV_STATE State) PURE;

    STDMETHOD(Reset)(THIS_ _In_ VDEV_STATE State) PURE;

    /* Once every other device has reset as well */
    STDMETHOD(PostReset)(THIS_ _In_ VDEV_STATE State) PURE;
};

/* HOW A DEVICE IS REACHED ****************************************************/

DEFINE_GUID(IID_IVndIoPortHandler,
            0x52604d3a, 0xb620, 0x4cb7, 0xab, 0x32, 0x99, 0x50, 0xe6, 0xf4, 0xa8, 0x09);

/*
 * A guest access to one of the ports a device reserved. Which ports those are
 * is not asked for here: the device hands over a range and a pointer to this at
 * the time it reserves them.
 */
#undef INTERFACE
#define INTERFACE IVndIoPortHandler
DECLARE_INTERFACE_(IVndIoPortHandler, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /* The range this was reserved for has been given up */
    STDMETHOD(NotifyUnregistered)(THIS) PURE;

    STDMETHOD(NotifyIoPortRead)(THIS_ _In_ USHORT Port,
                                _In_ USHORT Width,
                                _Out_ PULONG Value) PURE;
    STDMETHOD(NotifyIoPortWrite)(THIS_ _In_ USHORT Port,
                                 _In_ USHORT Width,
                                 _In_ ULONG Value) PURE;
};

DEFINE_GUID(IID_IVndMmioHandler,
            0xdcf3c21f, 0xa132, 0x4220, 0xa2, 0x15, 0x48, 0x3e, 0xcb, 0x01, 0xc0, 0x0a);

/*
 * The same for a window of guest memory. A device that owns one is not handed
 * the memory: the window is left out of what the guest has, and every access
 * to it arrives here instead.
 */
#undef INTERFACE
#define INTERFACE IVndMmioHandler
DECLARE_INTERFACE_(IVndMmioHandler, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(NotifyUnregistered)(THIS) PURE;

    STDMETHOD(NotifyMmioRead)(THIS_ _In_ ULONG64 Address,
                              _In_ ULONG Length,
                              _Out_writes_bytes_(Length) PVOID Buffer) PURE;
    STDMETHOD(NotifyMmioWrite)(THIS_ _In_ ULONG64 Address,
                               _In_ ULONG Length,
                               _In_reads_bytes_(Length) const VOID *Buffer) PURE;
};

DEFINE_GUID(IID_IVmTimerHandler,
            0xfe96de2e, 0xbb67, 0x4c7e, 0x84, 0x5f, 0xf3, 0xe3, 0x06, 0x9d, 0xfc, 0xab);

struct IVmTimer;

/*
 * A device that asked to be woken at a time, being woken. Which of its timers
 * went off is named, because a device may hold several and the display holds
 * four.
 */
#undef INTERFACE
#define INTERFACE IVmTimerHandler
DECLARE_INTERFACE_(IVmTimerHandler, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(OnTimerExpired)(THIS_ _In_ struct IVmTimer *Which) PURE;
};

/* WHAT ONE DEVICE OFFERS ANOTHER *********************************************/

DEFINE_GUID(IID_IVmPicService,
            0x81a7b678, 0x73b5, 0x4188, 0xae, 0x42, 0x88, 0x2b, 0xfc, 0xdc, 0x75, 0x62);

/*
 * The interrupt controller, as everything that raises a line sees it. A device
 * is handed one of these among its services and never learns anything else
 * about where its line goes.
 *
 * A line is shared, so raising it names both the line and which of the devices
 * on it is raising it. The line only falls once the last of them has let go,
 * which is why letting go names the raiser as well.
 */
#undef INTERFACE
#define INTERFACE IVmPicService
DECLARE_INTERFACE_(IVmPicService, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(EndOfInterrupt)(THIS) PURE;
    STDMETHOD(AssertIrq)(THIS_ _In_ UCHAR Line,
                         _In_ UCHAR Source) PURE;
    STDMETHOD(DeassertIrq)(THIS_ _In_ UCHAR Line,
                           _In_ UCHAR Source) PURE;
};

/* How many devices can hold one line up at once, and the one nothing shares */
#define VDEV_IRQ_SOURCES 32
#define VDEV_IRQ_SOURCE_ONLY 0

/* The line the second controller hangs off, which nothing else may raise */
#define VDEV_IRQ_CASCADE 2

DEFINE_GUID(IID_IVmDmaController,
            0xbce7fce2, 0x3bc8, 0x4c2c, 0xa2, 0xc8, 0x27, 0x6f, 0x51, 0x1a, 0x24, 0x24);

/*
 * The transfer controller, as a device that moves data without the processor
 * sees it. The device never learns where in memory anything went: that is the
 * whole point of the channel having been programmed by somebody else.
 */
#undef INTERFACE
#define INTERFACE IVmDmaController
DECLARE_INTERFACE_(IVmDmaController, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(GetDmaChannelCount)(THIS_ _Out_ PULONG Count) PURE;

    /*
     * Asks for a turn on a channel. Nothing is moved here: what comes back is
     * where in guest memory the transfer goes and how much of it the channel
     * was programmed to carry, and the device that asked does the moving. That
     * is why a controller needs no way of its own to reach memory.
     *
     * The third argument is passed in a floating point register and nothing
     * yet says what it is for. It is declared so that everything after it
     * lands where it belongs.
     */
    STDMETHOD(RequestDma)(THIS_ _In_ ULONG Channel,
                          _In_ double Unknown,
                          _In_ ULONG Length,
                          _Out_ PULONG Direction,
                          _Out_ PULONG64 Address,
                          _Out_ PULONG Count,
                          _Out_ PULONG Result) PURE;

    STDMETHOD(ReportDmaComplete)(THIS_ _In_ ULONG Channel) PURE;
};

DEFINE_GUID(IID_IVmPitService,
            0xc8d6e99d, 0xae82, 0x4b49, 0xa9, 0xb0, 0x7f, 0xc7, 0x5a, 0x04, 0x7c, 0x62);

/*
 * The counters, as something wired to one of them sees them. Nothing asks what
 * a count is: what a speaker or a rate measurement wants to know is whether an
 * output is high, and that is the only question this answers.
 */
#undef INTERFACE
#define INTERFACE IVmPitService
DECLARE_INTERFACE_(IVmPitService, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(EnableSpeakerTimer)(THIS_ _In_ BOOL Enabled) PURE;
    STDMETHOD(GetTimerOutputSignal)(THIS_ _In_ ULONG Counter,
                                    _Out_ PBOOL High) PURE;
};

DEFINE_GUID(IID_IVmPciConfigAccessHandler,
            0x8d181706, 0xcb35, 0x49a4, 0x95, 0x25, 0x7d, 0x14, 0x99, 0x18, 0x61, 0x71);

/*
 * A guest reading or writing the bytes that describe one device on the bus.
 *
 * The bus never holds those bytes. It works out which device is being asked
 * about and passes the question on, which is why a device that moves to a
 * different place on the bus needs nothing changed inside it.
 *
 * Offset is always a whole four bytes in, and the value is always four bytes
 * wide. A narrower access is widened by the bus before it arrives here, and a
 * narrower write is read back and merged there too, so a device is never asked
 * to take a register apart.
 */
#undef INTERFACE
#define INTERFACE IVmPciConfigAccessHandler
DECLARE_INTERFACE_(IVmPciConfigAccessHandler, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(NotifyPciConfigAccess)(THIS_ _In_ UCHAR Bus,
                                     _In_ UCHAR Device,
                                     _In_ UCHAR Function,
                                     _In_ USHORT Offset,
                                     _In_ UCHAR Writing,
                                     _Inout_ PULONG Value) PURE;
};

DEFINE_GUID(IID_IVmInstalledPciDevice,
            0x57040f7e, 0xab05, 0x4191, 0x8a, 0xb2, 0x25, 0x84, 0xec, 0xd6, 0x89, 0x6a);

/*
 * What a device is given back once it is on the bus. A line raised through
 * this is the one the guest routed to that place on the bus, which is not
 * something the device is ever told.
 */
#undef INTERFACE
#define INTERFACE IVmInstalledPciDevice
DECLARE_INTERFACE_(IVmInstalledPciDevice, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(AssertPciIrq)(THIS_ _In_ UCHAR Pin, _In_ ULONG64 Reserved) PURE;
    STDMETHOD(DeassertPciIrq)(THIS_ _In_ UCHAR Pin, _In_ ULONG64 Reserved) PURE;
};

DEFINE_GUID(IID_IVmPciBusService,
            0xd90779f1, 0x0fbe, 0x4d28, 0xb4, 0x2d, 0x16, 0xfc, 0xeb, 0x5e, 0xa7, 0x0c);

/*
 * The bus, as a device that wants to be found on it sees it. A device asks for
 * a place and hands over what answers for the bytes that describe it; where
 * those two ports are and how a question arrives at them is not its business.
 */
#undef INTERFACE
#define INTERFACE IVmPciBusService
DECLARE_INTERFACE_(IVmPciBusService, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(InstallPciDevice)(THIS_ _In_ IVmPciConfigAccessHandler *Handler,
                                _In_ UCHAR Device,
                                _In_ UCHAR Function,
                                _Outptr_opt_ IVmInstalledPciDevice **Installed) PURE;
};

/* Where the two ports every one of these questions goes through answer */
#define VDEV_PCI_ADDRESS_PORT   0x0CF8
#define VDEV_PCI_RESET_PORT     0x0CF9
#define VDEV_PCI_DATA_PORT      0x0CFC

/* The rest of these, named but not yet laid out */
DEFINE_GUID(IID_IVmSuperIo,
            0x060604ae, 0x6a0b, 0x4e03, 0x8a, 0xfe, 0x25, 0xfc, 0xa6, 0x8c, 0xb5, 0xd1);
DEFINE_GUID(IID_IVmInputController,
            0x5a753463, 0xf272, 0x4d06, 0xa5, 0x53, 0xfb, 0x47, 0xc1, 0x36, 0x28, 0x38);

/* WHAT THE MANAGER OWES A DEVICE *********************************************/

DEFINE_GUID(IID_IVmServiceAccess,
            0x20beef08, 0xc3ab, 0x44d8, 0x92, 0xc3, 0x03, 0xec, 0x0c, 0xf3, 0x98, 0xdc);

/*
 * How a device reaches everything else. It is given one of these when it is
 * initialised and asks it for each of the services it said it needed.
 */
#undef INTERFACE
#define INTERFACE IVmServiceAccess
DECLARE_INTERFACE_(IVmServiceAccess, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(GetService)(THIS_ _In_ REFIID Service,
                          _Outptr_ PVOID *Object) PURE;
};

DEFINE_GUID(IID_IVmAmd64EmulationServices,
            0xfcace8d2, 0xab0d, 0x480d, 0xb9, 0x79, 0x55, 0xc2, 0xda, 0x5f, 0x95, 0x79);

/*
 * What comes back from reserving ports or memory, and the only thing that
 * gives the reservation up again.
 *
 * A device holds one of these for as long as it answers for what it reserved.
 * When it is switched off it revokes and then lets go, in that order, and
 * whatever it was handed has to survive both: handing it back its own handler
 * makes the second of those two calls the one that frees the device.
 */
#undef INTERFACE
#define INTERFACE IVndRegistration
DECLARE_INTERFACE_(IVndRegistration, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(Revoke)(THIS) PURE;
};

/*
 * The one every device wants, and where the addresses it answers for are asked
 * for. What comes back from either of the two registering calls is what gives
 * the reservation up again.
 */
#undef INTERFACE
#define INTERFACE IVmAmd64EmulationServices
DECLARE_INTERFACE_(IVmAmd64EmulationServices, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /* A window of guest memory, counted in pages rather than bytes */
    STDMETHOD(RegisterMmioHandler)(THIS_ _In_ ULONG64 FirstPage,
                                   _In_ ULONG64 PageCount,
                                   _In_ IVndMmioHandler *Handler,
                                   _In_ BOOL Enabled,
                                   _Outptr_ IVndRegistration **Registration) PURE;

    /* The mailbox, and the cycle that says an interrupt has been finished */
    STDMETHOD(RegisterMbHandler)(THIS) PURE;
    STDMETHOD(RegisterApicEoiHandler)(THIS) PURE;

    /*
     * A run of ports, from the first to the last inclusive. Widths is a mask of
     * the access sizes the device will answer for, and every device seen asks
     * for all of them.
     */
    STDMETHOD(RegisterIoPortHandler)(THIS_ _In_ USHORT FirstPort,
                                     _In_ USHORT LastPort,
                                     _In_ ULONG Widths,
                                     _In_ IVndIoPortHandler *Handler,
                                     _In_ ULONG Flags,
                                     _Outptr_ IVndRegistration **Registration) PURE;

    /* A machine specific register, and a fault, taken by a device */
    STDMETHOD(RegisterMsrHandler)(THIS) PURE;
    STDMETHOD(RegisterExceptionHandler)(THIS) PURE;
};

/* Every access size there is, which is what a device asks for */
#define VDEV_WIDTH_ANY 31

/* What a memory window is counted in, because it is never counted in bytes */
#define VDEV_PAGE_SIZE 0x1000

DEFINE_GUID(IID_IVmProcessorServices,
            0x5f662e9d, 0x2097, 0x4eb5, 0x85, 0x27, 0x65, 0x8b, 0xa5, 0x4a, 0xc0, 0x49);

/*
 * The processors, as a device that has something for them sees them. The
 * interrupt controller works out which vector is owed and says so through here;
 * whether the guest is willing to take one is not its problem.
 */
#undef INTERFACE
#define INTERFACE IVmProcessorServices
DECLARE_INTERFACE_(IVmProcessorServices, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(GetVirtualProcessorCount)(THIS_ _Out_ PULONG Count) PURE;
    STDMETHOD(SetVirtualProcessorState)(THIS) PURE;
    STDMETHOD(GetVirtualProcessorState)(THIS) PURE;

    /*
     * A vector the processors should take. Delivery is how it arrives, and a
     * vector of all ones means there is nothing owed any more.
     */
    STDMETHOD(AssertVirtualProcessorInterrupt)(THIS_ _In_ ULONG64 Delivery,
                                               _In_ ULONG64 Reserved,
                                               _In_ ULONG Vector) PURE;

    /*
     * Clears what was asserted, and succeeds only if it had been taken. That
     * is the handshake a real controller gets from the cycle that acknowledges
     * an interrupt, and what tells it to put the vector in service.
     */
    STDMETHOD(ClearVirtualProcessorInterrupt)(THIS) PURE;

    STDMETHOD(ConfigureInterceptThrottlingExclusion)(THIS) PURE;
    STDMETHOD(StopAllVirtualProcessors)(THIS) PURE;
    STDMETHOD(StartAllVirtualProcessors)(THIS) PURE;
};

/*
 * How an interrupt is delivered, as the local controller in a processor numbers
 * it. The pair of chips arrives as the outside one, which is what makes the
 * processor ask for a vector rather than be given one.
 */
#define VDEV_DELIVERY_FIXED     0
#define VDEV_DELIVERY_LOWEST    1
#define VDEV_DELIVERY_SMI       2
#define VDEV_DELIVERY_NMI       4
#define VDEV_DELIVERY_INIT      5
#define VDEV_DELIVERY_EXTERNAL  7

/*
 * What the spare argument of the call that asserts one carries besides who
 * it goes to. The call has three arguments and a line needs five things said
 * about it, so the two that do not fit ride in the high bits of that one.
 * Ours, and read back only by something that put them there.
 */
#define IOAPIC_SAID_LOGICAL     0x0100
#define IOAPIC_SAID_LEVEL       0x0200

/* That there is nothing owed */
#define VDEV_NO_VECTOR ((ULONG)-1)
DEFINE_GUID(IID_IVmIoApic,
            0x9d33829b, 0x58be, 0x4bbf, 0xab, 0x6e, 0x3b, 0x16, 0xdb, 0xce, 0xf9, 0x54);

/*
 * What a device raises its line on. Not the interrupt controller directly:
 * where a line goes depends on what the guest has set up, and only this knows.
 * On a machine whose guest has never set anything up it goes to the pair of
 * chips, which is why that pair is one of the things this asks for and is
 * content to be told it cannot have.
 */
#undef INTERFACE
#define INTERFACE IVmIoApic
DECLARE_INTERFACE_(IVmIoApic, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(WaitForIrqAssert)(THIS_ _In_ ULONG Line) PURE;

    /* Shared the same way the pair of chips shares them, and for the same reason */
    STDMETHOD(AssertIrq)(THIS_ _In_ UCHAR Line,
                         _In_ UCHAR Source) PURE;
    STDMETHOD(DeassertIrq)(THIS_ _In_ UCHAR Line,
                           _In_ UCHAR Source) PURE;

    /*
     * For a device whose line is a clock and would rather not be woken for it.
     * Assisted comes back saying the line will be raised without the device,
     * and StillWanted saying it has to keep raising it anyway.
     */
    STDMETHOD(RequestTimerAssist)(THIS_ _In_ UCHAR Line,
                                  _In_ ULONG64 Period,
                                  _Out_ PINT Assisted,
                                  _Out_ PINT StillWanted) PURE;
    STDMETHOD(DeclineTimerAssist)(THIS_ _In_ UCHAR Line) PURE;

    /* Being told when the guest changes where a line goes */
    STDMETHOD(RegisterRteChangeCallback)(THIS_ _In_ UCHAR Line,
                                         _In_ IUnknown *Callback) PURE;
    STDMETHOD(UnregisterRteChangeCallback)(THIS_ _In_ UCHAR Line) PURE;

    STDMETHOD(SetIoApicBaseAddress)(THIS_ _In_ ULONG Address) PURE;
};

/* Where the redirection table answers unless the guest moves it */
#define VDEV_IOAPIC_DEFAULT_BASE 0xFEC00000ull
DEFINE_GUID(IID_IVmGuestMemoryAccess,
            0x2461c824, 0x4e2a, 0x4848, 0xbb, 0x65, 0x57, 0x08, 0xb2, 0x7f, 0x06, 0xd9);

/*
 * The guest's memory, as a device that has to reach into it sees it. A device
 * that moves whole sectors or paints whole screens goes through here rather
 * than faulting on every byte.
 *
 * Most of what is here is for building the memory a guest has rather than for
 * reading it, and only a manager that lets its devices do that answers them.
 */
#undef INTERFACE
#define INTERFACE IVmGuestMemoryAccess
DECLARE_INTERFACE_(IVmGuestMemoryAccess, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /*
     * A run of memory for a device to keep its own contents in.
     *
     * What comes back is not memory the device can write to directly: it is a
     * thing standing for that memory, which the device then asks to be put
     * somewhere the guest can see. The display asks for one of these before it
     * asks for anything else, and will not come up without it.
     */
    STDMETHOD(CreateDeviceMemoryBlock)(THIS_ _In_ ULONG64 Pages,
                                       _In_ ULONG Kind,
                                       _In_ ULONG Flags,
                                       _Outptr_ IVmMemoryBlock **Block) PURE;

    STDMETHOD(CreateRamGpaRange)(THIS) PURE;
    STDMETHOD(CreateRamApertureFromByteRange)(THIS) PURE;
    STDMETHOD(CreateSectionBackedGpaRange)(THIS) PURE;
    STDMETHOD(CreateDaxFileBackedGpaRange)(THIS) PURE;
    STDMETHOD(RegisterForVtl2Access)(THIS) PURE;

    /* Reading and writing it, which is what an ordinary device wants */
    STDMETHOD(ReadRamBytes)(THIS_ _In_ ULONG64 Address,
                            _Out_writes_bytes_(Length) PVOID Buffer,
                            _In_ ULONG Length) PURE;
    STDMETHOD(WriteRamBytes)(THIS_ _In_ ULONG64 Address,
                             _In_reads_bytes_(Length) const VOID *Buffer,
                             _In_ ULONG Length) PURE;
    STDMETHOD(ReadRamBytesEx)(THIS) PURE;
    STDMETHOD(WriteRamBytesEx)(THIS) PURE;

    STDMETHOD(CreateNotificationWithHandler)(THIS) PURE;
    STDMETHOD(GetHclErrorPageLocations)(THIS) PURE;

    STDMETHOD(TranslateGvaToGpa)(THIS_ _In_ ULONG64 Address,
                                 _Out_ PULONG64 Physical) PURE;

    STDMETHOD(CreateMemoryBlockPageAperture)(THIS) PURE;
    STDMETHOD(DestroyAperture)(THIS) PURE;
    STDMETHOD(RegisterForEmulationOnMemoryWrite)(THIS) PURE;
    STDMETHOD(UnregisterEmulationOnMemoryWrite)(THIS) PURE;
};

DEFINE_GUID(IID_IVmBios,
            0x9be0b79f, 0x68df, 0x4c59, 0x9d, 0x88, 0x4b, 0xfc, 0x1b, 0xf7, 0xa7, 0x3d);

/*
 * The firmware, as a device that has to be in it sees it. A drive says it can
 * be booted from, a serial controller says it is there, and the clock's
 * contents come from here because the firmware is what decided them.
 */
#undef INTERFACE
#define INTERFACE IVmBios
DECLARE_INTERFACE_(IVmBios, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /* A device saying it has done something the firmware should know about */
    STDMETHOD(NotifyEmulatedActivity)(THIS) PURE;

    /* A drive saying it is one of the places this machine can be booted from */
    STDMETHOD(RegisterBootDevice)(THIS_ _In_ ULONG Kind) PURE;

    /* And a serial controller saying which of the ports is there */
    STDMETHOD(EnableSerialController)(THIS_ _In_ UCHAR Which) PURE;

    /*
     * What the clock of the machine holds before the guest has written any of
     * it, which the firmware decided and the clock itself does not know.
     */
    STDMETHOD(GetDefaultCmosValues)(THIS_ _Out_ PUCHAR Values) PURE;

    STDMETHOD(IsGuestHibernateEnabled)(THIS_ _Out_ PINT Enabled) PURE;
    STDMETHOD(SaveShutdownType)(THIS_ _In_ ULONG Why) PURE;
};

/* How much of the clock of the machine the firmware has an opinion about */
#define VDEV_CMOS_DEFAULTS 128

DEFINE_GUID(IID_IMonitorDevice,
            0x0cf78153, 0xff01, 0x4af8, 0x8e, 0xe0, 0x1b, 0x3b, 0x44, 0x54, 0xfc, 0x11);

/* Which of the displays a machine may have is being talked about */
typedef ULONG VDEV_VIDEO_KIND;

/* The one this machine has, being the card rather than anything synthetic */
#define VDEV_VIDEO_S3 1

/*
 * What a display device is given to work against.
 *
 * Not a callback, which is what the name suggests and what this was taken for
 * at first. It is the other way round: the thing looking at the machine owns
 * the memory the picture is in, and a display device asks it for that memory,
 * tells it how much of it the card is supposed to have, and then draws into it.
 * The S3 device asks for four megabytes and will not come up without them.
 *
 * The slots past the ones named are what the shipped monitor has and nothing
 * here calls, kept so that a device reaching one of them is reaching the right
 * place rather than off the end of the table.
 */
#undef INTERFACE
#define INTERFACE IMonitorDevice
DECLARE_INTERFACE_(IMonitorDevice, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(GetThumbnailImage)(THIS_ _In_opt_ PVOID Repository,
                                 _In_ USHORT Width,
                                 _In_ USHORT Height,
                                 _In_ ULONG Flags,
                                 _Outptr_ PVOID *Image) PURE;
    STDMETHOD(RequestBitmap)(THIS_ _In_ RECT Where,
                             _In_ ULONG Pitch,
                             _In_ int Depth,
                             _Out_ PUCHAR Pixels,
                             _In_ ULONG Length,
                             _In_ int Which) PURE;

    /* The memory the picture lives in, which the display device draws into */
    STDMETHOD(GetVramBaseAddress)(THIS_ _Outptr_ PUCHAR *Base) PURE;
    STDMETHOD(GetVramSize)(THIS_ _Out_ PULONG Size) PURE;
    STDMETHOD(GetVramMemoryBlock)(THIS_ _Outptr_ IVmMemoryBlock **Block) PURE;
    STDMETHOD(IsVramAllocated)(THIS) PURE;
    STDMETHOD(ClearVram)(THIS) PURE;

    /* How much of it the card is supposed to have, said before it is asked for */
    STDMETHOD(SetMemoryRequired)(THIS_ _In_ ULONG Bytes) PURE;
    STDMETHOD(SetMemoryForSave)(THIS_ _In_ ULONG Bytes) PURE;

    STDMETHOD(RegisterVideoSource)(THIS_ _In_ IVideoVdev *Display,
                                   _In_ int Which) PURE;

    STDMETHOD(GetDisplaySettings)(THIS_ _Out_ PULONG Width,
                                  _Out_ PULONG Height,
                                  _Out_ PULONG Depth) PURE;

    STDMETHOD(GetPointerPosition)(THIS_ _Out_ PINT Across,
                                  _Out_ PINT Down) PURE;
    STDMETHOD(GetPointerShape)(THIS_ _Out_ PVOID Shape) PURE;

    STDMETHOD(GetActiveDeviceType)(THIS_ _Out_ VDEV_VIDEO_KIND *Which) PURE;
    STDMETHOD(GetClientCount)(THIS_ _Out_ PULONG Count) PURE;

    STDMETHOD(SetMonitorVideoActive)(THIS_ _In_ int Active,
                                     _In_ ULONG Which) PURE;
    STDMETHOD(RegisterSyntheticMouse)(THIS_ _In_ IVmMouseDevice *Mouse) PURE;

    STDMETHOD(OnClientCountChanged)(THIS) PURE;
    STDMETHOD(OnDisplaySettingsChanged)(THIS) PURE;
};

DEFINE_GUID(IID_IVmKeyboardDevice,
            0x7acc77f4, 0xdaca, 0x4d2e, 0xa0, 0xce, 0x83, 0x9a, 0xb6, 0xab, 0x42, 0x2f);

/*
 * One keystroke, as the wire between a keyboard and a machine carries it.
 *
 * The number is the one the key sends rather than anything about what is
 * printed on it, and whether it is going down or coming up is a flag rather
 * than a separate number, which is how a keyboard of this kind has always
 * said it.
 */
typedef struct _VDEV_KEYSTROKE
{
    USHORT Unit;
    USHORT Code;
    USHORT Flags;
    USHORT Reserved;
    ULONG Extra;
} VDEV_KEYSTROKE, *PVDEV_KEYSTROKE;

/* Going down is nothing at all, and the rest say the key is not a plain one */
#define VDEV_KEY_DOWN       0x0000
#define VDEV_KEY_UP         0x0001
#define VDEV_KEY_EXTENDED   0x0002
#define VDEV_KEY_EXTENDED1  0x0004

/*
 * The keyboard, as whatever the operator is typing at reaches it. A controller
 * of this kind offers one of these for each of the things plugged into it.
 */
#undef INTERFACE
#define INTERFACE IVmKeyboardDevice
DECLARE_INTERFACE_(IVmKeyboardDevice, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(DeviceClass)(THIS_ _Out_ PULONG Class) PURE;
    STDMETHOD(DeviceState)(THIS_ _Out_ PULONG State) PURE;
    STDMETHOD(IsUnicodeSupported)(THIS_ _Out_ PINT Supported) PURE;
    STDMETHOD(LedState)(THIS_ _Out_ PUCHAR Lamps) PURE;

    /* The one that matters, being a key going down or coming up */
    STDMETHOD(SendKeystroke)(THIS_ _In_ PVDEV_KEYSTROKE Key) PURE;

    /* How many more it will take before it has nowhere to put them */
    STDMETHOD(GetOutputBufferFreeSpace)(THIS_ _Out_ PULONG Free) PURE;
};

DEFINE_GUID(IID_IRtvmVideoWatcher,
            0x9c3a5f21, 0x7d84, 0x4e0b, 0xb1, 0xf6, 0x2a, 0x55, 0xc8, 0xd1, 0x04, 0x73);

/*
 * Ours, and nobody else's: being told that a display stopped being what it was.
 *
 * The shipped display device does not do this. It is handed memory and writes
 * into it, and whatever is looking at that memory works out for itself when to
 * draw. Ours says so instead, because a window that redraws only when something
 * changed costs nothing to write and a great deal less to run.
 */
#undef INTERFACE
#define INTERFACE IRtvmVideoWatcher
DECLARE_INTERFACE_(IRtvmVideoWatcher, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(OnVideoDirt)(THIS_ _In_ VDEV_VIDEO_KIND Which) PURE;
};

DEFINE_GUID(IID_IVideoVdev,
            0x1401754a, 0xf009, 0x4b06, 0xb8, 0xf4, 0xef, 0x08, 0xeb, 0x57, 0x2d, 0x98);

/*
 * What a display is told about the surface it is drawing. Four numbers, of
 * which the middle two are certainly how big it is; the first and last are
 * written from fields whose meaning is not yet known, and are named for what
 * a surface of this shape would carry there.
 */
typedef struct _VDEV_SURFACE_DATA
{
    ULONG Format;
    ULONG Width;
    ULONG Height;
    ULONG Pitch;
} VDEV_SURFACE_DATA, *PVDEV_SURFACE_DATA;

C_ASSERT(sizeof(VDEV_SURFACE_DATA) == 16);

/* The display, as whatever draws it asks after being told something changed */
#undef INTERFACE
#define INTERFACE IVideoVdev
DECLARE_INTERFACE_(IVideoVdev, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(IsVideoEnabled)(THIS_ _Out_ PBOOL Enabled) PURE;
    STDMETHOD(Activate)(THIS) PURE;
    STDMETHOD(GetSurfaceData)(THIS_ _Out_ PVDEV_SURFACE_DATA Surface) PURE;
};

/*
 * Ours, and past everything the reference has. A display here draws characters
 * rather than pixels, and there is no call in any of the above that hands one
 * over: the reference reaches the memory behind a surface another way, which
 * is not yet worked out. Anything built against the reference ignores this,
 * because it is asked for by an identifier nothing there has.
 */
/*
 * How a device here is told what to be. The reference hands a device a
 * repository in Initialize and it reads what it needs out of that; there is no
 * such thing here yet, so this stands in. Anything built against the reference
 * never sees it, because it is asked for by an identifier nothing there has.
 */
DEFINE_GUID(IID_IRtvmDeviceSettings,
            0x41b6e0c7, 0x9d52, 0x4f83, 0xb1, 0x0e, 0x37, 0x8a, 0x2c, 0x64, 0xd9, 0x1f);

DEFINE_GUID(IID_IRtvmApertureServices,
            0x6f2ad814, 0x73be, 0x4c05, 0x9e, 0x21, 0x84, 0x0d, 0x5b, 0x37, 0xc6, 0x92);

/*
 * Memory a device owns that the guest reaches as ordinary memory rather than
 * by faulting into the device for every access.
 *
 * The reference has this among the calls that build the memory a guest has,
 * and none of those are laid out here yet, so this stands in for the one of
 * them a display needs. A screen made of pixels is written a whole frame at a
 * time, and a window that took an exit for every pixel would never draw one.
 */
#undef INTERFACE
#define INTERFACE IRtvmApertureServices
DECLARE_INTERFACE_(IRtvmApertureServices, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /* Somewhere above everything the guest was given as memory */
    STDMETHOD(CreateAperture)(THIS_ _In_ ULONG64 Base,
                              _In_ ULONG64 Length,
                              _Outptr_result_maybenull_ PVOID *Where) PURE;
};

DEFINE_GUID(IID_IRtvmTextSurface,
            0x8d2f4a61, 0x5c3e, 0x4b17, 0x9a, 0x44, 0x1e, 0x7d, 0x62, 0x0b, 0xc8, 0x35);

DEFINE_GUID(IID_IRtvmPixelSurface,
            0x2c7a90e4, 0x6b18, 0x4d5a, 0xa3, 0x61, 0x9f, 0x04, 0xe8, 0x35, 0x71, 0xda);

/*
 * What the first of the four numbers means here. The reference writes it from
 * a field whose meaning is not yet known, so these are this project's own and
 * are only ever read back by something that asked for one of our interfaces.
 */
#define VDEV_SURFACE_TEXT       0
#define VDEV_SURFACE_INDEXED    1
#define VDEV_SURFACE_DIRECT     2

#undef INTERFACE
#define INTERFACE IRtvmDeviceSettings
DECLARE_INTERFACE_(IRtvmDeviceSettings, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /* Whatever followed the kind on the command line, before it comes up */
    STDMETHOD(SetSettings)(THIS_ _In_ PCSTR Settings) PURE;
};

#undef INTERFACE
#define INTERFACE IRtvmTextSurface
DECLARE_INTERFACE_(IRtvmTextSurface, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /* A character and the colour it is drawn in, per cell, row by row */
    STDMETHOD(ReadCells)(THIS_ _Out_writes_bytes_(Length) PVOID Cells,
                         _In_ ULONG Length,
                         _Out_ PULONG CursorColumn,
                         _Out_ PULONG CursorRow) PURE;
};

/*
 * The same again for a screen made of pixels rather than characters. What is
 * handed over is one byte per pixel whatever the guest arranged behind it,
 * because how the parts of a colour are laid out in the device is the device's
 * own business and no display has ever wanted to know.
 */
#undef INTERFACE
#define INTERFACE IRtvmPixelSurface
DECLARE_INTERFACE_(IRtvmPixelSurface, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /*
     * A run of rows, each of them the surface's own pitch long. One byte to a
     * pixel where the surface says its colours are named below, and four where
     * it says each pixel carries its own.
     */
    STDMETHOD(ReadRows)(THIS_ _In_ ULONG First,
                        _In_ ULONG Count,
                        _Out_writes_bytes_(Length) PVOID Rows,
                        _In_ ULONG Length) PURE;

    /* And what each of those bytes stands for, as three parts of six bits */
    STDMETHOD(ReadPalette)(THIS_ _Out_writes_bytes_(Length) PVOID Colours,
                           _In_ ULONG Length) PURE;
};

DEFINE_GUID(IID_IProxiedPciVgaDevice,
            0xfcb3759f, 0xd139, 0x46be, 0x85, 0x00, 0x5c, 0x50, 0xa6, 0xfb, 0xff, 0x9b);

DEFINE_GUID(IID_IVmTimeSource,
            0xe162fe7a, 0x72c6, 0x4d0e, 0x93, 0xdd, 0x7d, 0xf9, 0x1a, 0x5b, 0x97, 0x9d);

/*
 * THE CLOCK, AND WHAT IS ASKED OF IT
 *
 * A device that has anything to do over time does not keep a thread. It asks
 * the clock for a timer of its own, hands over something to be called back
 * through, and says when it wants to hear from it. Several kinds cannot come up
 * at all without one: the interval timer, the clock of the machine itself, the
 * transfer controller and the display all name it.
 *
 * What a tick is worth is not stated anywhere. The one thing that is is that the
 * same clock says what the time is now and is told when to go off, so whoever
 * provides it decides, as long as it is consistent with itself.
 */

/* One timer, as the device that asked for it sees it */
#undef INTERFACE
#define INTERFACE IVmTimer
DECLARE_INTERFACE_(IVmTimer, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    /* What the time is now, in whatever the clock counts in */
    STDMETHOD(GetTime)(THIS_ _Out_ PULONG64 Now) PURE;

    STDMETHOD(Reserved4)(THIS) PURE;
    STDMETHOD(Reserved5)(THIS) PURE;
    STDMETHOD(Reserved6)(THIS) PURE;
    STDMETHOD(Reserved7)(THIS) PURE;

    /* When to go off, and how often after that */
    STDMETHOD(Arm)(THIS_ _In_ ULONG64 Kind,
                   _In_ ULONG64 Period,
                   _In_ ULONG64 Due,
                   _In_ ULONG Repeating) PURE;

    STDMETHOD(Reserved9)(THIS) PURE;
    STDMETHOD(Reserved10)(THIS) PURE;

    STDMETHOD(Cancel)(THIS_ _In_ ULONG64 Why) PURE;
};

#undef INTERFACE
#define INTERFACE IVmTimeSource
DECLARE_INTERFACE_(IVmTimeSource, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(Reserved3)(THIS) PURE;
    STDMETHOD(Reserved4)(THIS) PURE;

    STDMETHOD(CreateTimer)(THIS_ _In_ IVmTimerHandler *Handler,
                           _Outptr_ IVmTimer **Timer) PURE;
};

/* What the clock here counts in, being ten million of them to the second */
#define VDEV_TICKS_A_SECOND 10000000ull
DEFINE_GUID(IID_IVmPowerServices,
            0x3ee9144c, 0x27d7, 0x4c8e, 0xa0, 0x7e, 0x5d, 0xd5, 0xf7, 0xa0, 0x20, 0x7d);

/*
 * THE REST OF WHAT A MACHINE OF THIS KIND HAS
 *
 * None of these is asked for by anything here. They are the services the parts
 * that do the most on such a machine cannot come up without, and a device that
 * names one and is refused it says so and stops, so they are worth naming even
 * while there is nothing behind them: a list of what is missing is the list of
 * what has to be built.
 */

/* What the memory of a machine looks like, before the guest and after */
DEFINE_GUID(IID_IVmBootMemoryTopology,
            0xb80fe14e, 0xb5f6, 0x43d4, 0xb2, 0x06, 0x40, 0xb3, 0xbf, 0x51, 0x19, 0x59);
DEFINE_GUID(IID_IVmMemoryTopology,
            0x4f99e8b7, 0x37bc, 0x4ee4, 0xb5, 0x39, 0x50, 0x26, 0x3b, 0x47, 0x83, 0xb6);
DEFINE_GUID(IID_IVmMemoryManagement,
            0xe7bb1d35, 0xad97, 0x464b, 0x8a, 0x3f, 0x95, 0xf4, 0x3e, 0x0f, 0x43, 0x89);

/* The channel a guest reaches everything synthetic through */
DEFINE_GUID(IID_IVmbusServices,
            0xece3f556, 0xf87f, 0x4120, 0x9e, 0x37, 0xaa, 0xa5, 0x5e, 0x5e, 0x0c, 0xa9);
DEFINE_GUID(IID_IVpciServices,
            0xc8769be0, 0x2c2b, 0x4ded, 0xbd, 0x3c, 0xff, 0x75, 0x15, 0xd7, 0x4e, 0x90);

/* The partition itself, and who is allowed to ask it for what */
DEFINE_GUID(IID_IVmPartitionServices,
            0x773e9a95, 0x1b2d, 0x4479, 0x95, 0x5f, 0x40, 0x20, 0x00, 0xeb, 0xe6, 0xc2);
DEFINE_GUID(IID_ISecurityManager,
            0x5315507b, 0x19f0, 0x4e86, 0xab, 0x51, 0x18, 0xf1, 0x59, 0xf1, 0xa1, 0x97);
DEFINE_GUID(IID_IVmManagementAccess,
            0xbb011455, 0xa4f6, 0x4e08, 0x99, 0x82, 0x09, 0xaf, 0xd3, 0x03, 0xdf, 0x20);
DEFINE_GUID(IID_IVmHandleBrokerServices,
            0xe9e61d12, 0xa2c3, 0x4e55, 0xac, 0x35, 0xb8, 0xf2, 0x6d, 0x21, 0x6a, 0x69);

/* Coming back into a state that was written out, and writing one out */
DEFINE_GUID(IID_IVmBootStateImporter,
            0x034e6428, 0x672e, 0x403a, 0xa3, 0x42, 0x4f, 0x4c, 0x8d, 0x6a, 0x70, 0x5c);
DEFINE_GUID(IID_IVmGuestStateAccess,
            0x96ddf97a, 0x0b79, 0x4966, 0x8b, 0x56, 0x74, 0x0f, 0x0d, 0x76, 0x6e, 0x2e);
DEFINE_GUID(IID_IVmGuestStateRawStorage,
            0xf299b139, 0x1550, 0x4327, 0x84, 0xf7, 0xc1, 0xf4, 0x33, 0x25, 0x8e, 0xef);

/* What is kept when a guest stops the way it should not have */
DEFINE_GUID(IID_IVmGuestCrashServices,
            0x4f80e76e, 0x0d0f, 0x44e8, 0x87, 0xbd, 0x02, 0x80, 0xe1, 0x79, 0x93, 0x51);
DEFINE_GUID(IID_IVmCrashRegisterServices,
            0xf0109dc7, 0x3f96, 0x41b1, 0xb0, 0xbc, 0x5a, 0xea, 0x91, 0x1c, 0x40, 0x4c);

/* And the parts a machine has whether or not anything is using them */
DEFINE_GUID(IID_IVmPowerManagementDevice,
            0x3f60da8b, 0xe8ef, 0x403a, 0x81, 0x73, 0x9e, 0xf5, 0xc6, 0xee, 0x01, 0x52);
DEFINE_GUID(IID_IVmBattery,
            0x2a811607, 0xc21c, 0x47da, 0x84, 0xa0, 0x3c, 0x3b, 0x29, 0xaa, 0xd4, 0xe4);
DEFINE_GUID(IID_IVpmemController,
            0x521087ab, 0x2963, 0x4859, 0xb6, 0xd9, 0xd6, 0xf1, 0xec, 0x9f, 0x33, 0x82);
DEFINE_GUID(IID_IVmPsp,
            0xb0c36d19, 0x3f91, 0x4b3d, 0xb8, 0xdc, 0xee, 0xe5, 0xbb, 0x2c, 0x9a, 0xba);

#undef INTERFACE

#ifdef __cplusplus
}
#endif
