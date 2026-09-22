/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Terminal device arrival and departure tracking
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Providers such as the display stack report the monitors and input devices
 * of a session here. ReactOS runs a single terminal, so devices are only
 * tracked until they leave, when the provider's close routine is called.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

/* The routines a provider hands over for a device, each called with its token */
typedef struct _TTMP_DEVICE_ROUTINES
{
    VOID (NTAPI *CloseDevice)(_In_ ULONG64 Token);
    NTSTATUS (NTAPI *TerminalAssigned)(_In_ ULONG64 Token, _In_ ULONG TerminalId);
    NTSTATUS (NTAPI *DisplayStateChange)(_In_ ULONG64 Token, _In_ ULONG DisplayState);
    NTSTATUS (NTAPI *InputModeChange)(_In_ ULONG64 Token, _In_ ULONG InputMode);
} TTMP_DEVICE_ROUTINES, *PTTMP_DEVICE_ROUTINES;

#define TTMP_MAX_IDENTITY_CHARS     260

typedef struct _TTMP_DEVICE
{
    LIST_ENTRY Link;
    ULONG Provider;
    ULONG64 Token;
    LONG DeviceId;
    ULONG DeviceType;
    TTMP_DEVICE_ROUTINES Routines;
    WORK_QUEUE_ITEM CloseWorkItem;
    WCHAR Identity[TTMP_MAX_IDENTITY_CHARS];
} TTMP_DEVICE, *PTTMP_DEVICE;

/* GLOBALS ********************************************************************/

static LIST_ENTRY TtmpDeviceList = { &TtmpDeviceList, &TtmpDeviceList };
static EX_PUSH_LOCK TtmpDeviceLock;
static LONG TtmpLastDeviceId;

/* PRIVATE FUNCTIONS **********************************************************/

static
PTTMP_DEVICE
TtmpFindDevice(
    _In_ ULONG Provider,
    _In_ ULONG64 Token)
{
    PLIST_ENTRY Entry;
    PTTMP_DEVICE Device;

    for (Entry = TtmpDeviceList.Flink; Entry != &TtmpDeviceList; Entry = Entry->Flink)
    {
        Device = CONTAINING_RECORD(Entry, TTMP_DEVICE, Link);
        if ((Device->Provider == Provider) && (Device->Token == Token))
            return Device;
    }

    return NULL;
}

static
VOID
NTAPI
TtmpCloseDeviceWorker(
    _In_ PVOID Context)
{
    PTTMP_DEVICE Device = Context;

    Device->Routines.CloseDevice(Device->Token);
    ExFreePoolWithTag(Device, TAG_TTM_DEVICE);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Reports a device that a provider brought to the current terminal.
 *
 * @param[in] Provider
 * The kind of provider reporting the device.
 *
 * @param[in] Token
 * The provider's value for the device, handed back to its routines.
 *
 * @param[in] Routines
 * The provider's routines for the device. The close routine is required.
 *
 * @param[in] DeviceType
 * The provider specific type of the device.
 *
 * @param[in] Identity
 * Optional NUL terminated name of the device, at most 259 characters.
 *
 * @return
 * STATUS_SUCCESS, STATUS_DEVICE_ALREADY_ATTACHED if the token is already
 * known, STATUS_INVALID_PARAMETER, or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
NTAPI
TtmNotifyDeviceArrival(
    _In_ ULONG Provider,
    _In_ ULONG64 Token,
    _In_ const VOID *Routines,
    _In_ ULONG DeviceType,
    _In_opt_ PCUNICODE_STRING Identity)
{
    const TTMP_DEVICE_ROUTINES *DeviceRoutines = Routines;
    PTTMP_DEVICE Device = NULL;
    size_t IdentityChars;
    NTSTATUS Status;

    PAGED_CODE();

    if ((DeviceRoutines == NULL) || (DeviceRoutines->CloseDevice == NULL))
        return STATUS_INVALID_PARAMETER;

    if (Identity)
    {
        Status = RtlStringCchLengthW(Identity->Buffer, TTMP_MAX_IDENTITY_CHARS, &IdentityChars);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&TtmpDeviceLock);

    if (TtmpFindDevice(Provider, Token))
    {
        Status = STATUS_DEVICE_ALREADY_ATTACHED;
        goto Quit;
    }

    Device = ExAllocatePoolZero(PagedPool, sizeof(*Device), TAG_TTM_DEVICE);
    if (Device == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Quit;
    }

    Device->Provider = Provider;
    Device->Token = Token;
    Device->DeviceId = InterlockedIncrement(&TtmpLastDeviceId);
    Device->DeviceType = DeviceType;
    Device->Routines = *DeviceRoutines;

    if (Identity)
    {
        Status = RtlStringCchCopyW(Device->Identity, TTMP_MAX_IDENTITY_CHARS, Identity->Buffer);
        if (!NT_SUCCESS(Status))
            goto Quit;
    }

    InsertTailList(&TtmpDeviceList, &Device->Link);
    Device = NULL;
    Status = STATUS_SUCCESS;

Quit:
    ExReleasePushLockExclusive(&TtmpDeviceLock);
    KeLeaveCriticalRegion();

    if (Device)
        ExFreePoolWithTag(Device, TAG_TTM_DEVICE);

    return Status;
}

/**
 * @brief
 * Reports that a device reported through TtmNotifyDeviceArrival is gone.
 *
 * @param[in] Provider
 * The kind of provider that reported the device.
 *
 * @param[in] Token
 * The provider's value for the device.
 *
 * @remarks
 * The provider's close routine runs later from a worker thread, since the
 * caller may hold locks that routine needs.
 */
VOID
NTAPI
TtmNotifyDeviceDeparture(
    _In_ ULONG Provider,
    _In_ ULONG64 Token)
{
    PTTMP_DEVICE Device;

    PAGED_CODE();

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&TtmpDeviceLock);

    Device = TtmpFindDevice(Provider, Token);
    if (Device)
        RemoveEntryList(&Device->Link);

    ExReleasePushLockExclusive(&TtmpDeviceLock);
    KeLeaveCriticalRegion();

    if (Device == NULL)
        return;

    ExInitializeWorkItem(&Device->CloseWorkItem, TtmpCloseDeviceWorker, Device);
    ExQueueWorkItem(&Device->CloseWorkItem, DelayedWorkQueue);
}

/* EOF */
