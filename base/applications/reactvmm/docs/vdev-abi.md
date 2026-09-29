# The virtual device contract

ReacTVmm hosts emulated devices the same way the Hyper-V worker process does, so
that a device library written for one can be loaded by the other. This file is
the map of that contract: what an interface is called, what it is identified by,
and what sits in each of its slots.

Everything here was read out of the shipped binaries with a debugger and their
public symbols. It describes an interface, not an implementation; how any of it
behaves inside is written separately and owes nothing to theirs.

## The module is a class server

A device library is a COM in-process server. It exports:

    DllGetClassObject
    DllCanUnloadNow
    DllRegisterServer

and nothing else that the host reaches by name. Each kind of device is a class
with its own CLSID, and the host asks for one by that.

## What an object looks like

Devices are aggregatable, so each object carries an inner and an outer identity.
The inner one sits at offset zero and is the controlling unknown; the interfaces
the device publishes are sub-objects at fixed offsets inside it.

`PicDevice`, as an example of the shape:

| Offset | Interface                                      |
|--------|------------------------------------------------|
| 0      | the inner unknown                              |
| 24     | the virtual device base                        |
| 264    | `IVmPicService`                                |
| 272    | `IVndIoPortHandler`                            |

The inner unknown's slots are not `IUnknown`'s. They are:

| Slot | Method                      |
|------|-----------------------------|
| 0    | `InnerQueryInterface`       |
| 1    | `InnerAddRef`               |
| 2    | `InnerRelease`              |
| 3    | vector deleting destructor  |
| 4    | `OnAddRefBounce`            |
| 5    | `OnReleaseUnderflow`        |

## The virtual device base

`{0693ed7d-8a8a-4d87-a468-1103b8c63d9c}`

Every device has this, and the host drives its whole life through it. The first
three slots are `IUnknown`.

| Slot | Method                        | What it is for                                        |
|------|-------------------------------|-------------------------------------------------------|
| 0    | `QueryInterface`              |                                                       |
| 1    | `AddRef`                      |                                                       |
| 2    | `Release`                     |                                                       |
| 3    | `GetDependencies`             | Which services the device cannot do without           |
| 4    | `Initialize`                  | Handed those services                                 |
| 5    | `Teardown`                    | Gives them back                                       |
| 6    | `StartReservingResources`     | Asks for its ports, memory windows and lines          |
| 7    | `FinishReservingResources`    | They are now its own                                  |
| 8    | `FreeReservedResources`       |                                                       |
| 9    | `SaveReservedResources`       |                                                       |
| 10   | `PowerOnCold`                 | Coming up from nothing                                |
| 11   | `PowerOnRestore`              | Coming up into a state that was saved                 |
| 12   | `PowerOff`                    |                                                       |
| 13   | `Save`                        | Writes its state out                                  |
| 14   | `Resume`                      |                                                       |
| 15   | `Pause`                       |                                                       |
| 16   | `EnableOptimizations`         |                                                       |
| 17   | `StartDisableOptimizations`   |                                                       |
| 18   | `FinishDisableOptimizations`  |                                                       |
| 19   | `Reset`                       |                                                       |
| 20   | `PostReset`                   | Once everything else has reset too                    |
| 21   | vector deleting destructor    |                                                       |

The dependency list is part of the type, not of the object: a device names the
services it wants in its own declaration, and `GetDependencies` reports them. The
interrupt controller asks only for `IVmAmd64EmulationServices` and
`IVmProcessorServices`. The firmware loader asks for around twenty.

## How a port reaches a device

`IVndIoPortHandler` `{52604d3a-b620-4cb7-ab32-9950e6f4a809}`

| Slot | Method                |
|------|-----------------------|
| 0    | `QueryInterface`      |
| 1    | `AddRef`              |
| 2    | `Release`             |
| 3    | (not yet identified)  |
| 4    | `NotifyIoPortRead`    |
| 5    | `NotifyIoPortWrite`   |

Which ports reach it is not asked for through this interface. Each device class
carries a static table of port ranges, a pair of sixteen bit numbers per entry
being the first and last port of one range. The device walks that table when it
comes up and reserves each range in turn through the emulation service, handing
over a pointer to its own `IVndIoPortHandler` sub-object and keeping what comes
back so it can be given up later.

The call is slot six of `IVmAmd64EmulationServices`:

    HRESULT (IVmAmd64EmulationServices *Service,
             USHORT FirstPort,
             USHORT LastPort,
             ULONG Widths,
             IVndIoPortHandler *Handler,
             ULONG Flags,
             PVOID *Registration)

`Widths` is a mask of the access sizes the device will answer for, and every
device seen so far passes 31, which is all of them. `Flags` is zero everywhere.
The transfer controller registers eighteen ranges this way and the video device
two, each into its own slot of an array the device keeps.

`IVndMmioHandler` is the same idea for a window of memory.

## What one device offers another

A device that other devices need does not go through the host. It publishes an
interface, and whoever needs it is given a pointer to it as one of its
dependencies. The interrupt controller is the clearest case.

`IVmPicService` `{81a7b678-73b5-4188-ae42-882bfcdc7562}`

| Slot | Method             |
|------|--------------------|
| 0    | `QueryInterface`   |
| 1    | `AddRef`           |
| 2    | `Release`          |
| 3    | `EndOfInterrupt`   |
| 4    | `AssertIrq`        |
| 5    | `DeassertIrq`      |

The others of this kind, not yet laid out here: `IVmPitService`,
`IVmDmaController`, `IVmIoApic`, `IVmPciBusService`, `IVmSuperIo`,
`IVmKeyboardDevice`, `IVmMouseDevice`, `IPs2MouseDevice`, `IVideoVdev`,
`ISerialDevice`, `ISerialPortDevice`, `IVmBios`, `IVmBattery`, `IVmPsp`,
`IVmPowerManagementDevice`, `IVmTimerHandler`, `IVmPciConfigAccessHandler`.

## What the host owes a device

These are named in the dependency lists and are the host's to implement. Until
they exist nothing loads.

`IVmAmd64EmulationServices` is wanted by almost every device and is where ports
and memory windows are reserved. After it, by how often they are asked for:
`IVmProcessorServices`, `IVmIoApic`, `IVmTimeSource`, `IVmGuestMemoryAccess`,
`ISecurityManager`, `IVmPowerServices`, `IVmPartitionServices`,
`IVmMemoryTopology`, `IVmBootMemoryTopology`, `IVmManagementAccess`,
`IVmHandleBrokerServices`, `IVmbusServices`.

A dependency may be wrapped in `OptionalService<>`, which is how a device says it
will come up without one.

## Which devices live where

| Library                | Devices                                                                                  |
|------------------------|------------------------------------------------------------------------------------------|
| emulated devices       | interrupt controller, interval timer, transfer controller, keyboard controller, speaker, video, PCI bus, ISA bus |
| chipset                | real time clock, IO APIC, firmware loader, guest emulation                                |
| serial                 | serial controller and its ports                                                           |
| emulated storage       | the disks on the wire                                                                     |
| emulated network       | the card on the wire                                                                      |
| user interface devices | the mouse and the parts an operator touches                                               |

ReacTVmm's own libraries are to line up with these rather than with how it is
convenient to write them.

## Where ReacTVmm is against this

Nothing yet. What exists is a flat C vtable reached through one exported entry
point, with the interrupt controller and the transfer controller built into the
manager instead of being devices at all. The emulation inside each of them is
sound and carries over; the plumbing around them does not.

The order to fix it in:

1. Class server plumbing, so a library is loaded by CLSID rather than by name.
2. The virtual device base, and the inner and outer identities around it.
3. `IVmAmd64EmulationServices`, enough of it to reserve ports.
4. The interrupt controller as a device publishing `IVmPicService`, moved out of
   the manager.
5. Load the shipped emulated device library and bring its interrupt controller up
   in place of ours. That is the test that says whether any of this is right.
6. The transfer controller, the interval timer and the rest, the same way.
