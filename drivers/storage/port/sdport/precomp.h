/*
 * PROJECT:     ReactOS Storage Stack / SD host controller port driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Internal definitions
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#ifndef _SDPORT_PRECOMP_H_
#define _SDPORT_PRECOMP_H_

#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdmguid.h>
#include <sdport.h>

#define SDPORT_POOL_TAG 'troS'
#define SDPORT_MAX_SLOTS 8

/* Event bits reported by the miniport interrupt callback. */
#define SDPORT_EVENT_CARD_CHANGE 0x000000C0
#define SDPORT_EVENT_SDIO        0x00000100
#define SDPORT_EVENT_TUNING      0x00001000

typedef enum _SDPORT_EXTENSION_TYPE
{
    SdportExtensionFdo = 1,
    SdportExtensionPdo = 2
} SDPORT_EXTENSION_TYPE;

typedef enum _SDPORT_DEVICE_STATE
{
    SdportStateStopped = 0,
    SdportStateStarting = 1,
    SdportStateStarted = 2,
    SdportStateRemoved = 3
} SDPORT_DEVICE_STATE;

typedef struct _SDPORT_DRIVER_EXTENSION
{
    PDRIVER_OBJECT DriverObject;
    UNICODE_STRING RegistryPath;
    SDPORT_INITIALIZATION_DATA InitializationData;
    BOOLEAN SynchronousStartDevice;
} SDPORT_DRIVER_EXTENSION, *PSDPORT_DRIVER_EXTENSION;

typedef struct _SDPORT_SLOT_EXTENSION SDPORT_SLOT_EXTENSION, *PSDPORT_SLOT_EXTENSION;

typedef struct _SDPORT_FDO_EXTENSION
{
    SDPORT_EXTENSION_TYPE ExtensionType;
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Pdo;
    PDEVICE_OBJECT LowerDevice;
    PSDPORT_DRIVER_EXTENSION DriverExtension;
    PSDPORT_INITIALIZATION_DATA InitData;
    BUS_INTERFACE_STANDARD BusInterface;
    BOOLEAN BusInterfaceValid;
    UCHAR SlotCount;
    PSDPORT_SLOT_EXTENSION Slots[SDPORT_MAX_SLOTS];
    PVOID InterruptObject;
    PDMA_ADAPTER DmaAdapter;
    IO_REMOVE_LOCK RemoveLock;
    SDPORT_DEVICE_STATE State;
    BOOLEAN CrashdumpMode;
    KDPC CompletionDpc;
    KDPC IsrDpc;
    KTIMER PollTimer;
    KDPC PollDpc;
    BOOLEAN PollActive;
    KSPIN_LOCK PollLock;
    LONG PollOutstanding;
} SDPORT_FDO_EXTENSION, *PSDPORT_FDO_EXTENSION;

struct _SDPORT_SLOT_EXTENSION
{
    SDPORT_EXTENSION_TYPE ExtensionType;
    UCHAR SlotNumber;
    PDEVICE_OBJECT Self;
    PSDPORT_FDO_EXTENSION FdoExtension;
    PVOID PrivateExtension;
    PHYSICAL_ADDRESS PhysicalBase;
    PVOID VirtualBase;
    ULONG Length;
    SDPORT_CAPABILITIES Capabilities;
    volatile LONG EnabledEvents;
    volatile LONG PendingEvents;
    volatile LONG IsrEvents;
    volatile LONG IsrErrors;
    BOOLEAN CrashdumpMode;
    BOOLEAN WriteProtected;
    BOOLEAN Present;
    UCHAR BusWidth;
    ULONG ClockFrequencyKhz;
    SDPORT_BUS_SPEED BusSpeed;
    USHORT Rca;
    BOOLEAN HighCapacity;
    BOOLEAN CardInitialized;
    UCHAR Cid[16];
    UCHAR Csd[16];
    IO_REMOVE_LOCK RemoveLock;
    SDPORT_DEVICE_STATE State;
    KSPIN_LOCK QueueLock;
    LIST_ENTRY ActiveRequests;
    KEVENT DeviceInterfaceEvent;
    UNICODE_STRING DeviceInterface;
    BOOLEAN DeviceInterfaceEnabled;
};

/* Per-request state owned by the port driver.  The public request is the first
   member so SdPortCompleteRequest can recover the wrapper via CONTAINING_RECORD. */
typedef struct _SDPORT_WORK_REQUEST
{
    SDPORT_REQUEST Public;
    PSDPORT_SLOT_EXTENSION Slot;
    PIRP Irp;
    PSDPORT_REQUEST UserRequest;
    LIST_ENTRY QueueEntry;
    KEVENT CompletionEvent;
    KTIMER TimeoutTimer;
    KDPC TimeoutDpc;
    BOOLEAN Synchronous;
    BOOLEAN Internal;
    BOOLEAN TimerArmed;
    BOOLEAN MdlLocked;
    volatile LONG CompletionQueued;
    PMDL DataMdl;
} SDPORT_WORK_REQUEST, *PSDPORT_WORK_REQUEST;

#define SdPortWorkFromPublic(Req) \
    CONTAINING_RECORD((Req), SDPORT_WORK_REQUEST, Public)

/* sdport.c */
DRIVER_ADD_DEVICE SdPortAddDevice;
DRIVER_UNLOAD SdPortUnload;

/* pnp.c */
NTSTATUS
NTAPI
SdPortDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
SdPortStartSlots(
    _In_ PSDPORT_FDO_EXTENSION FdoExtension);

/* request.c */
NTSTATUS
NTAPI
SdPortDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
SdPortDispatchInternalDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

KSERVICE_ROUTINE SdPortInterruptRoutine;
KDEFERRED_ROUTINE SdPortCompletionDpc;
KDEFERRED_ROUTINE SdPortIsrDpc;
KDEFERRED_ROUTINE SdPortPollDpc;

NTSTATUS
NTAPI
SdPortGetProperty(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _Inout_ PSDPORT_REQUEST Request);

NTSTATUS
NTAPI
SdPortIssueSynchronousRequest(
    _In_ PSDPORT_SLOT_EXTENSION Slot,
    _Inout_ PSDPORT_REQUEST Request);

/* init_card.c */
NTSTATUS
NTAPI
SdPortInitializeCard(
    _In_ PSDPORT_SLOT_EXTENSION Slot);

/* power.c */
NTSTATUS
NTAPI
SdPortDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
SdPortDispatchSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

#endif /* _SDPORT_PRECOMP_H_ */
