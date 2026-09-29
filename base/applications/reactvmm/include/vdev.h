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

    /* Which services it cannot come up without, as a list of their identifiers */
    STDMETHOD(GetDependencies)(THIS_ _In_opt_ PVOID Repository,
                               _Out_ PULONG Count,
                               _Outptr_ GUID **Services,
                               _Out_ PULONG Optional) PURE;

    /* Handed its configuration and something to reach those services through */
    STDMETHOD(Initialize)(THIS_ _In_opt_ PVOID Repository,
                          _In_ ULONG_PTR Reserved,
                          _In_ IUnknown *Provider) PURE;
    STDMETHOD(Teardown)(THIS) PURE;

    /* Where it asks for the ports, the memory and the lines it answers for */
    STDMETHOD(StartReservingResources)(THIS) PURE;
    STDMETHOD(FinishReservingResources)(THIS) PURE;
    STDMETHOD(FreeReservedResources)(THIS) PURE;
    STDMETHOD(SaveReservedResources)(THIS) PURE;

    /* Coming up from nothing, or into a state that was written out before */
    STDMETHOD(PowerOnCold)(THIS) PURE;
    STDMETHOD(PowerOnRestore)(THIS) PURE;
    STDMETHOD(PowerOff)(THIS) PURE;
    STDMETHOD(Save)(THIS) PURE;
    STDMETHOD(Resume)(THIS) PURE;
    STDMETHOD(Pause)(THIS) PURE;

    STDMETHOD(EnableOptimizations)(THIS) PURE;
    STDMETHOD(StartDisableOptimizations)(THIS) PURE;
    STDMETHOD(FinishDisableOptimizations)(THIS) PURE;

    STDMETHOD(Reset)(THIS) PURE;

    /* Once every other device has reset as well */
    STDMETHOD(PostReset)(THIS) PURE;
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
                                _In_ ULONG Width,
                                _Out_ PULONG Value) PURE;
    STDMETHOD(NotifyIoPortWrite)(THIS_ _In_ USHORT Port,
                                 _In_ ULONG Width,
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

/* A device that asked to be woken at a time, being woken */
#undef INTERFACE
#define INTERFACE IVmTimerHandler
DECLARE_INTERFACE_(IVmTimerHandler, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(OnTimerExpired)(THIS) PURE;
};

/* WHAT ONE DEVICE OFFERS ANOTHER *********************************************/

DEFINE_GUID(IID_IVmPicService,
            0x81a7b678, 0x73b5, 0x4188, 0xae, 0x42, 0x88, 0x2b, 0xfc, 0xdc, 0x75, 0x62);

/*
 * The interrupt controller, as everything that raises a line sees it. A device
 * is handed one of these among its services and never learns anything else
 * about where its line goes.
 */
#undef INTERFACE
#define INTERFACE IVmPicService
DECLARE_INTERFACE_(IVmPicService, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(EndOfInterrupt)(THIS_ _In_ ULONG Line) PURE;
    STDMETHOD(AssertIrq)(THIS_ _In_ ULONG Line) PURE;
    STDMETHOD(DeassertIrq)(THIS_ _In_ ULONG Line) PURE;
};

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

/* The rest of these, named but not yet laid out */
DEFINE_GUID(IID_IVmPciBusService,
            0xd90779f1, 0x0fbe, 0x4d28, 0xb4, 0x2d, 0x16, 0xfc, 0xeb, 0x5e, 0xa7, 0x0c);
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
                                   _Outptr_ PVOID *Registration) PURE;

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
                                     _Outptr_ PVOID *Registration) PURE;

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
    STDMETHOD(AssertIrq)(THIS_ _In_ ULONG Line) PURE;
    STDMETHOD(DeassertIrq)(THIS_ _In_ ULONG Line) PURE;

    /* For a device whose line is a clock and would rather not be woken for it */
    STDMETHOD(RequestTimerAssist)(THIS_ _In_ ULONG Line) PURE;
    STDMETHOD(DeclineTimerAssist)(THIS_ _In_ ULONG Line) PURE;

    /* Being told when the guest changes where a line goes */
    STDMETHOD(RegisterRteChangeCallback)(THIS_ _In_ ULONG Line,
                                         _In_ IUnknown *Callback) PURE;
    STDMETHOD(UnregisterRteChangeCallback)(THIS_ _In_ ULONG Line,
                                           _In_ IUnknown *Callback) PURE;

    STDMETHOD(SetIoApicBaseAddress)(THIS_ _In_ ULONG64 Address) PURE;
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

    /* Making memory, and the windows onto it */
    STDMETHOD(CreateDeviceMemoryBlock)(THIS) PURE;
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

    STDMETHOD(NotifyEmulatedActivity)(THIS) PURE;
    STDMETHOD(RegisterBootDevice)(THIS) PURE;
    STDMETHOD(EnableSerialController)(THIS) PURE;
    STDMETHOD(GetDefaultCmosValues)(THIS) PURE;
    STDMETHOD(IsGuestHibernateEnabled)(THIS) PURE;
    STDMETHOD(SaveShutdownType)(THIS) PURE;
};

DEFINE_GUID(IID_IMonitorDevice,
            0x0cf78153, 0xff01, 0x4af8, 0x8e, 0xe0, 0x1b, 0x3b, 0x44, 0x54, 0xfc, 0x11);

/*
 * Whatever the operator is looking at, as the video device tells it what has
 * changed. The display is not handed a screen: it is told which part of one
 * stopped being what it was, and fetches as much of it as it wants to draw.
 */
#undef INTERFACE
#define INTERFACE IMonitorDevice
DECLARE_INTERFACE_(IMonitorDevice, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ _In_ REFIID Interface,
                              _Outptr_ PVOID *Object) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;

    STDMETHOD(OnVideoDirt)(THIS_ _In_ const RECT *Changed) PURE;
    STDMETHOD(OnPointerShapeChanged)(THIS) PURE;
    STDMETHOD(OnPointerPositionChanged)(THIS) PURE;
    STDMETHOD(OnActivationRequested)(THIS) PURE;
    STDMETHOD(OnDeactivationRequested)(THIS) PURE;
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

DEFINE_GUID(IID_IRtvmTextSurface,
            0x8d2f4a61, 0x5c3e, 0x4b17, 0x9a, 0x44, 0x1e, 0x7d, 0x62, 0x0b, 0xc8, 0x35);

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

DEFINE_GUID(IID_IProxiedPciVgaDevice,
            0xfcb3759f, 0xd139, 0x46be, 0x85, 0x00, 0x5c, 0x50, 0xa6, 0xfb, 0xff, 0x9b);

DEFINE_GUID(IID_IVmTimeSource,
            0xe162fe7a, 0x72c6, 0x4d0e, 0x93, 0xdd, 0x7d, 0xf9, 0x1a, 0x5b, 0x97, 0x9d);
DEFINE_GUID(IID_IVmPowerServices,
            0x3ee9144c, 0x27d7, 0x4c8e, 0xa0, 0x7e, 0x5d, 0xd5, 0xf7, 0xa0, 0x20, 0x7d);

#undef INTERFACE

#ifdef __cplusplus
}
#endif
