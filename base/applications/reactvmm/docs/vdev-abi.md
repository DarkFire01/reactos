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
| 3    | `GetDependencies(repo, *count, **services, *required)` | Which services it wants     |
| 4    | `Initialize(repo, reserved, provider)` | Handed those services                |
| 5    | `Teardown()`                  | Gives them back                                       |
| 6    | `StartReservingResources(repo, state)` | Moves a state along, mostly          |
| 7    | `FinishReservingResources(state)` |                                                   |
| 8    | `FreeReservedResources()`     |                                                       |
| 9    | `SaveReservedResources(repo)`  |                                                      |
| 10   | `PowerOnCold(state)`          | Coming up from nothing, and where ports are asked for |
| 11   | `PowerOnRestore(repo, state)` | Coming up into a state that was saved                 |
| 12   | `PowerOff(state)`             | Revokes every reservation and lets it go              |
| 13   | `Save(repo, state)`           | Writes its state out                                  |
| 14   | `Resume(state)`               |                                                       |
| 15   | `Pause(state)`                |                                                       |
| 16   | `EnableOptimizations(state)`  |                                                       |
| 17   | `StartDisableOptimizations(state)` |                                                  |
| 18   | `FinishDisableOptimizations(state)` |                                                |
| 19   | `Reset(state)`                |                                                       |
| 20   | `PostReset(state)`            | Once everything else has reset too                    |
| 21   | vector deleting destructor    |                                                       |

The slot order was read off the interrupt controller's own table, and every entry
above is the method that binary has in that slot.

`state` is a mask carried into every step. Almost nothing reads it, and the two
places that do test one bit each, so a machine that has never been saved passes
nothing.

Nothing is reserved at slot 6. The base class there only records which state the
device was in and moves it on; the ports and memory windows are asked for at slot
10, and given back at slot 12. A host that stops after slot 7 sees a device that
answered for nothing and reports success.

The dependency list is part of the type, not of the object: a device names the
services it wants in its own declaration, and `GetDependencies` reports them. The
interrupt controller asks for `IVmAmd64EmulationServices` and
`IVmProcessorServices`. The firmware loader asks for twenty.

`required` is a count and not a mask of which. The answer is ordered, with the
services the device cannot come up without first and the ones it will do without
last, and `required` says where the break is. The optional ones are filled in from
the back, so they come out in the reverse of the order the device declared them.

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
             IVndRegistration **Registration)

`Widths` is a mask of the access sizes the device will answer for, and every
device seen so far passes 31, which is all of them. `Flags` is zero everywhere.
The transfer controller registers eighteen ranges this way and the video device
two, each into its own slot of an array the device keeps.

What comes back is an object of its own, with `IUnknown` and then one more slot
that gives the range up:

| Slot | Method           |
|------|------------------|
| 0    | `QueryInterface` |
| 1    | `AddRef`         |
| 2    | `Release`        |
| 3    | `Revoke`         |

A device being switched off calls slot 3 and then slot 2, in that order, on each
one it holds. Handing it back its own handler instead of a separate object makes
that second call a `Release` on the device, which frees it while the host is
still driving it.

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
| 3    | `EndOfInterrupt()`             |
| 4    | `AssertIrq(line, source)`      |
| 5    | `DeassertIrq(line, source)`    |

Both of the last two take a line and a source, each one byte. A line is shared, so
`source` says which of the devices on it is raising it: the controller keeps a
32-bit mask per line and the line only falls once the last of them has let go.
Letting go without naming the raiser drops the line for whichever of the others
was still waiting to be looked at.

`EndOfInterrupt` names nothing. Each of the two chips finishes whichever of its
lines it had in service, which is the lowest numbered one.

Line two is where the second chip hangs off the first, so nothing else may raise
it: a device that did would look to the first chip exactly like the second one
asking. The shipped controller asserts on that line rather than carrying it.

`IVmIoApic` takes a line and a source the same way. Its `WaitForIrqAssert` takes a
whole word, its `RequestTimerAssist` takes a line, a period and two places to
write to, and its `UnregisterRteChangeCallback` takes only the line.

The others of this kind, not yet laid out here: `IVmPitService`,
`IVmDmaController`, `IVmIoApic`, `IVmPciBusService`, `IVmSuperIo`,
`IVmKeyboardDevice`, `IVmMouseDevice`, `IPs2MouseDevice`, `IVideoVdev`,
`ISerialDevice`, `ISerialPortDevice`, `IVmBios`, `IVmBattery`, `IVmPsp`,
`IVmPowerManagementDevice`, `IVmTimerHandler`, `IVmPciConfigAccessHandler`.

## What each device is called

The class map in the shipped emulated device library, in the order it lists them:

| Class                     | Identifier                               |
|---------------------------|------------------------------------------|
| interrupt controller      | `{9edd1639-9bca-40dc-b3a2-07c828da60b5}` |
| PCI bus                   | `{84535fad-4d98-4a6a-bdcd-21d5720dc430}` |
| ISA bus                   | `{4d42d9f7-6531-4f6c-9e46-1f0477876104}` |
| speaker                   | `{4d46d139-7821-4dc4-98a3-01e98a586a44}` |
| super IO                  | `{35b0b12f-a0d7-482f-80a0-f52f1ab3da2e}` |
| interval timer            | `{a28e4d02-3323-4148-9569-565930a5cb39}` |
| transfer controller       | `{87045ce9-5323-438f-93bb-1e83dcbce18e}` |
| video                     | `{7d80d3db-61ee-4879-8879-5609f1100ad0}` |
| keyboard controller       | `{655bc5c5-a784-46b7-81bc-e26328f7eb0e}` |

Each entry of that map is six pointers: the identifier, nothing, the routine
that makes the class object, a routine called as the library goes away, and the
two that write the class into the registry and take it out again.

## The drives

The emulated storage library offers two, and each is a **controller** that owns
whatever is attached to it rather than one device per medium:

| Class                     | Identifier                               |
|---------------------------|------------------------------------------|
| IDE controller            | `{83f8638b-8dca-4152-9eda-2ca8b33039b4}` |
| floppy controller         | `{8f0d2762-0b00-4e04-af4f-19010527cb93}` |

The IDE one publishes `IVirtualStorage`, `IVndIoPortHandler` and
`IVmPciConfigAccessHandler`, along with a run of interfaces for moving a machine
while it runs: `ISnapshottableStorage`, `IVirtualDeviceMigration`,
`ITransferableHandles`, `IDeferredPowerOnDevice`, `IVmRecoverableDevice`,
`IVmResourcePoolConsumer`, `IVmMetricDevice`, `IOnlineCompatibilityInfo`. The
floppy one publishes only `IVirtualStorage` and `IVndIoPortHandler`.

This project has one device per medium, which is the wrong shape: a drive that
takes whole commands and one that does not are two drives on one controller,
not two controllers.

## How a device is given its services

`Initialize` is handed the device's configuration, a reserved word, and an
unknown. The device asks that unknown for `IVmServiceAccess`
`{20beef08-c3ab-44d8-92c3-03ec0cf398dc}`, whose slot three takes an interface
identifier and gives back the service behind it. It then asks for each service
in its own dependency list in turn, and fails to come up if a service it called
for is missing and was not marked as one it could do without.

The identifiers, read out of four different devices' lists and agreeing in every
one:

| Service                     | Identifier                               |
|-----------------------------|------------------------------------------|
| `IVmAmd64EmulationServices` | `{fcace8d2-ab0d-480d-b979-55c2da5f9579}` |
| `IVmProcessorServices`      | `{5f662e9d-2097-4eb5-8527-658ba54ac049}` |
| `IVmIoApic`                 | `{9d33829b-58be-4bbf-ab6e-3b16dbcef954}` |
| `IVmTimeSource`             | `{e162fe7a-72c6-4d0e-93dd-7df91a5b979d}` |
| `IVmPciBusService`          | `{d90779f1-0fbe-4d28-b42d-16fceb5ea70c}` |
| `IVmPitService`             | `{c8d6e99d-ae82-4b49-a9b0-7fc75a047c62}` |
| `IVmPowerServices`          | `{3ee9144c-27d7-4c8e-a07e-5dd5f7a0207d}` |
| `IVmSuperIo`                | `{060604ae-6a0b-4e03-8afe-25fca68cb5d1}` |
| `IVmInputController`        | `{5a753463-f272-4d06-a553-fb47c1362838}` |

## How an interrupt reaches a processor

The interrupt controller does not wait to be asked. It works out which vector is
owed and says so through slot six of `IVmProcessorServices`:

    HRESULT (IVmProcessorServices *Service,
             ULONG64 Kind,
             ULONG64 Reserved,
             ULONG Vector)

`Kind` is seven for a line arriving through the controller and `Reserved` is
zero. A vector of all ones means there is nothing owed any more. Only a change
is reported, so a controller that says nothing is a controller whose answer has
not moved.

Putting that vector in service is a separate step, because on real hardware it
happens in the cycle that acknowledges the interrupt and there is no such cycle
here. The controller asks slot seven, which takes nothing:

    HRESULT (IVmProcessorServices *Service)

and puts the vector in service if the answer is a success. It is tested for
success rather than against a particular code, so a host that has not delivered
the interrupt yet has to answer with a failure. Answering `S_FALSE` puts every
offered vector straight into service, which blocks the line it came in on and
delivers nothing ever again.

### What the line does after it is taken

A line is triggered by an edge. What is owed is cleared when the processor
takes it and is not owed again until the wire goes up afresh, however long it
stays up in the meantime. A device that holds its line down while it is still
busy is the ordinary case: the removable drive holds its own from the moment a
command finishes until the bytes it left behind are read.

Putting the request back because the wire is still held is what a chip wired
for levels does, and it costs more than it looks. Measured on one boot from a
removable drive: one raise of line six delivered its vector 1,032,246 times,
against 239 with the request latched on the edge alone.

## What is known of the emulation service

Slot three takes a window of guest memory, counted in pages:

    HRESULT (IVmAmd64EmulationServices *Service,
             ULONG64 FirstPage,
             ULONG64 PageCount,
             IVndMmioHandler *Handler,
             BOOL Enabled,
             PVOID *Registration)

The video device reserves page 176 through it, which is the text window at
0xB0000. Slot six is the port call written out above. Slots four and five are
not yet understood and nothing seen so far calls them.

## The display

A video device is not asked for a screen and does not hand one over. It is given
whatever the operator is looking at as `IMonitorDevice`
`{0cf78153-ff01-4af8-8ee0-1b3b4454fc11}` and tells it what stopped being what it
was:

| Slot | Method                       |
|------|------------------------------|
| 3    | `OnVideoDirt`                |
| 4    | `OnPointerShapeChanged`      |
| 5    | `OnPointerPositionChanged`   |
| 6    | `OnActivationRequested`      |
| 7    | `OnDeactivationRequested`    |

Whatever is drawing then asks the device for as much of it as it wants, through
`IVideoVdev` `{1401754a-f009-4b06-b8f4-ef08eb572d98}`, whose slots are
`IsVideoEnabled`, `Activate` and `GetSurfaceData`.

That is the way round this project had it backwards. The page was handed up
whole and often; the display is meant to be told a rectangle and to come back
for it when it is ready.

## The firmware

`IVmBios` `{9be0b79f-68df-4c59-9d88-4bfc1bf7a73d}`, which a device that has to
be in the firmware's tables asks for:

| Slot | Method                     |
|------|----------------------------|
| 3    | `NotifyEmulatedActivity`   |
| 4    | `RegisterBootDevice`       |
| 5    | `EnableSerialController`   |
| 6    | `GetDefaultCmosValues`     |
| 7    | `IsGuestHibernateEnabled`  |
| 8    | `SaveShutdownType`         |

A drive says here that it can be booted from, and the clock's contents come from
here because the firmware is what decided them.

## Reaching the guest's memory

`IVmGuestMemoryAccess` `{2461c824-4e2a-4848-bb65-5708b27f06d9}`, seventeen
methods. Most of them build the memory a guest has, which is the manager's work;
the ones a device uses are `ReadRamBytes` and `WriteRamBytes` at slots nine and
ten, each taking an address, a buffer and a length, and `TranslateGvaToGpa` at
slot fifteen.

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
will come up without one. Those are the ones that come last in the answer and are
not counted in `required`.

The identifiers for those, read off the two devices that want the most of them by
matching the answer against the list in their own declaration:

| Service                    | Identifier                               |
|----------------------------|------------------------------------------|
| `IVmbusServices`           | `{ece3f556-f87f-4120-9e37-aaa55e5e0ca9}` |
| `IVmBootMemoryTopology`    | `{b80fe14e-b5f6-43d4-b206-40b3bf511959}` |
| `IVmMemoryTopology`        | `{4f99e8b7-37bc-4ee4-b539-50263b4783b6}` |
| `IVmMemoryManagement`      | `{e7bb1d35-ad97-464b-8a3f-95f43e0f4389}` |
| `IVmBootStateImporter`     | `{034e6428-672e-403a-a342-4f4c8d6a705c}` |
| `IVmPowerManagementDevice` | `{3f60da8b-e8ef-403a-8173-9ef5c6ee0152}` |
| `IVmManagementAccess`      | `{bb011455-a4f6-4e08-9982-09afd303df20}` |
| `ISecurityManager`         | `{5315507b-19f0-4e86-ab51-18f159f1a197}` |
| `IVmPartitionServices`     | `{773e9a95-1b2d-4479-955f-402000ebe6c2}` |
| `IVmHandleBrokerServices`  | `{e9e61d12-a2c3-4e55-ac35-b8f26d216a69}` |
| `IVmGuestCrashServices`    | `{4f80e76e-0d0f-44e8-87bd-0280e1799351}` |
| `IVmCrashRegisterServices` | `{f0109dc7-3f96-41b1-b0bc-5aea911c404c}` |
| `IVmGuestStateRawStorage`  | `{f299b139-1550-4327-84f7-c1f433258eef}` |
| `IVmGuestStateAccess`      | `{96ddf97a-0b79-4966-8b56-740f0d766e2e}` |
| `IVpciServices`            | `{c8769be0-2c2b-4ded-bd3c-ff7515d74e90}` |
| `IVpmemController`         | `{521087ab-2963-4859-b6d9-d6f1ec9f3382}` |
| `IVmBattery`               | `{2a811607-c21c-47da-84a0-3c3b29aad4e4}` |
| `IVmPsp`                   | `{b0c36d19-3f91-4b3d-b8dc-eee5bb2c9aba}` |

Two devices name overlapping sets in different orders, and both agree on every
identifier they share. The last slot of the guest emulation device's answer comes
out as `IProxiedPciVgaDevice`, whose identifier was already known from elsewhere,
which is what says the ordering was read the right way round.

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
