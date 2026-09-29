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

    /* Not yet understood, and here so that the two below keep their slots */
    STDMETHOD(Unknown3)(THIS) PURE;

    STDMETHOD(NotifyIoPortRead)(THIS_ _In_ USHORT Port,
                                _In_ ULONG Width,
                                _Out_ PULONG Value) PURE;
    STDMETHOD(NotifyIoPortWrite)(THIS_ _In_ USHORT Port,
                                 _In_ ULONG Width,
                                 _In_ ULONG Value) PURE;
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

/* The rest of these, named but not yet laid out */
DEFINE_GUID(IID_IVmPitService,
            0xc8d6e99d, 0xae82, 0x4b49, 0xa9, 0xb0, 0x7f, 0xc7, 0x5a, 0x04, 0x7c, 0x62);
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
    STDMETHOD(RegisterGpaRange)(THIS_ _In_ ULONG64 FirstPage,
                                _In_ ULONG64 PageCount,
                                _In_ PVOID Handler,
                                _In_ BOOL Enabled,
                                _Outptr_ PVOID *Registration) PURE;

    /* Not yet understood, and here so that the one below keeps its slot */
    STDMETHOD(Unknown4)(THIS) PURE;
    STDMETHOD(Unknown5)(THIS) PURE;

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
};

/* Every access size there is, which is what a device asks for */
#define VDEV_WIDTH_ANY 31

DEFINE_GUID(IID_IVmProcessorServices,
            0x5f662e9d, 0x2097, 0x4eb5, 0x85, 0x27, 0x65, 0x8b, 0xa5, 0x4a, 0xc0, 0x49);
DEFINE_GUID(IID_IVmIoApic,
            0x9d33829b, 0x58be, 0x4bbf, 0xab, 0x6e, 0x3b, 0x16, 0xdb, 0xce, 0xf9, 0x54);
DEFINE_GUID(IID_IVmTimeSource,
            0xe162fe7a, 0x72c6, 0x4d0e, 0x93, 0xdd, 0x7d, 0xf9, 0x1a, 0x5b, 0x97, 0x9d);
DEFINE_GUID(IID_IVmPowerServices,
            0x3ee9144c, 0x27d7, 0x4c8e, 0xa0, 0x7e, 0x5d, 0xd5, 0xf7, 0xa0, 0x20, 0x7d);

#undef INTERFACE

#ifdef __cplusplus
}
#endif
