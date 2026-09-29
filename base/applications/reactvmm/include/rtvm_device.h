/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The contract between the manager and a piece of emulated hardware
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A hardware module is a library the manager loads, which offers one or more
 * kinds of device. The manager makes an instance of a kind, the instance says
 * which addresses it answers for, and from then on every guest access to one of
 * those addresses arrives as a call.
 *
 * The other implementation of this idea puts each device behind a class object
 * and reaches it through interface pointers. The shape here is the same, a
 * module that publishes kinds and instances that carry a table of entry points,
 * but the table is a plain one: a device is a library, not an object server,
 * and nothing about emulating a serial port is made clearer by a registry key.
 */

#pragma once

#include "rtvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Raised when a module compiled against an older copy of this file would be
 * misread. Adding a field to the end of a structure does not raise it, because
 * every structure here carries its own size and is read only as far as that.
 */
#define RTVM_DEVICE_ABI_VERSION 1

typedef struct _RTVM_DEVICE RTVM_DEVICE, *PRTVM_DEVICE;

/*
 * Whether one of the tables below reaches far enough to carry the named entry,
 * and has one. Anything past what Size says belongs to a newer version of this
 * file than whoever filled the table in was built against.
 */
#define RTVM_CARRIES(Table, Type, Member)                                     \
    (((Table) != NULL) &&                                                     \
     ((Table)->Size >= FIELD_OFFSET(Type, Member) + sizeof((Table)->Member)) &&\
     ((Table)->Member != NULL))

/* Lines the interrupt controller knows about, for SetInterruptLine below */
#define RTVM_LINE_NONE  ((ULONG)-1)

/*
 * A screenful of characters, as the device that owns the display has it. Cells
 * is Columns * Rows pairs of a character and the colour it is drawn in, which
 * is the order the hardware itself keeps them in.
 */
typedef struct _RTVM_TEXT_PAGE
{
    ULONG Size;
    ULONG Columns;
    ULONG Rows;
    ULONG CursorColumn;
    ULONG CursorRow;
    BOOLEAN CursorVisible;
    const UCHAR *Cells;
} RTVM_TEXT_PAGE, *PRTVM_TEXT_PAGE;

/*
 * What the operator did, for Input below. A key arrives as the code set one
 * numbers it by, with the byte the wire prefixes it with, if any, in the high
 * half, so that both bytes reach the controller as one event.
 */
typedef enum _RTVM_INPUT_KIND
{
    RtvmInputKeyDown = 0,
    RtvmInputKeyUp
} RTVM_INPUT_KIND;

/*
 * What the manager hands a device so it can reach back out. It belongs to the
 * manager and stays valid for as long as the device does.
 */
typedef struct _RTVM_HOST_INTERFACE
{
    ULONG Size;

    /* The manager's own, handed back on every call below */
    PVOID Context;

    /*
     * Answer for a range of port addresses. Every guest access inside it comes
     * back through IoRead and IoWrite until the device is destroyed.
     */
    RTVM_STATUS
    (RTVMAPI *ClaimPortRange)(
        _In_ PVOID Context,
        _In_ PRTVM_DEVICE Device,
        _In_ USHORT FirstPort,
        _In_ USHORT PortCount);

    /* The same for a window of guest physical memory */
    RTVM_STATUS
    (RTVMAPI *ClaimMemoryRange)(
        _In_ PVOID Context,
        _In_ PRTVM_DEVICE Device,
        _In_ ULONG64 BaseAddress,
        _In_ ULONG64 Length);

    /* Hold a line on the interrupt controller up or let it go */
    RTVM_STATUS
    (RTVMAPI *SetInterruptLine)(
        _In_ PVOID Context,
        _In_ ULONG Line,
        _In_ BOOLEAN Asserted);

    /* For a device that moves data itself rather than a word at a time */
    RTVM_STATUS
    (RTVMAPI *ReadGuestMemory)(
        _In_ PVOID Context,
        _In_ ULONG64 Address,
        _Out_writes_bytes_(Length) PVOID Buffer,
        _In_ ULONG Length);

    RTVM_STATUS
    (RTVMAPI *WriteGuestMemory)(
        _In_ PVOID Context,
        _In_ ULONG64 Address,
        _In_reads_bytes_(Length) const VOID *Buffer,
        _In_ ULONG Length);

    /*
     * Ask to be called back once, after roughly this long. A device with a
     * clock of its own asks again from inside the callback. Zero cancels a
     * request that has not fired.
     */
    RTVM_STATUS
    (RTVMAPI *SetTimer)(
        _In_ PVOID Context,
        _In_ PRTVM_DEVICE Device,
        _In_ ULONG64 Nanoseconds);

    /* Where a device says what the operator would want to know */
    VOID
    (RTVMAPI *Log)(
        _In_ PVOID Context,
        _In_ RTVM_LOG_LEVEL Level,
        _In_ PCSTR Format,
        ...);

    /*
     * Put a page of text in front of the operator. Refused when the manager has
     * nothing to draw on, which leaves the device to do whatever else it would
     * have done with the page.
     */
    RTVM_STATUS
    (RTVMAPI *PresentText)(
        _In_ PVOID Context,
        _In_ const RTVM_TEXT_PAGE *Page);

    /*
     * Move a device's data through one of the transfer channels. The device
     * never learns where in memory it went, which is the whole point of the
     * channel having been programmed by somebody else.
     */
    RTVM_STATUS
    (RTVMAPI *MoveThroughChannel)(
        _In_ PVOID Context,
        _In_ ULONG Channel,
        _Inout_updates_bytes_(Length) PVOID Buffer,
        _In_ ULONG Length,
        _Out_ PULONG Moved);
} RTVM_HOST_INTERFACE, *PRTVM_HOST_INTERFACE;

/*
 * What a device offers the manager. Anything a device does not do is left NULL
 * and the manager answers for it: a read of an unclaimed width returns all ones
 * and a write goes nowhere, which is what the bus does with a missing device.
 */
typedef struct _RTVM_DEVICE_VTABLE
{
    ULONG Size;

    /* The machine is about to run. Claims made here are the ones that stand */
    RTVM_STATUS (RTVMAPI *Start)(_In_ PRTVM_DEVICE Device);

    /* The machine has stopped. Nothing else will be called until Start again */
    VOID (RTVMAPI *Stop)(_In_ PRTVM_DEVICE Device);

    /* Back to the state it powers on in, without being destroyed */
    VOID (RTVMAPI *Reset)(_In_ PRTVM_DEVICE Device);

    /* Give back everything. The device pointer is not used again after this */
    VOID (RTVMAPI *Destroy)(_In_ PRTVM_DEVICE Device);

    /*
     * A guest read of a claimed port. The device writes what it would have put
     * on the bus, in the low Width bytes.
     */
    RTVM_STATUS
    (RTVMAPI *IoRead)(
        _In_ PRTVM_DEVICE Device,
        _In_ USHORT Port,
        _In_ ULONG Width,
        _Out_ PULONG Value);

    RTVM_STATUS
    (RTVMAPI *IoWrite)(
        _In_ PRTVM_DEVICE Device,
        _In_ USHORT Port,
        _In_ ULONG Width,
        _In_ ULONG Value);

    RTVM_STATUS
    (RTVMAPI *MemoryRead)(
        _In_ PRTVM_DEVICE Device,
        _In_ ULONG64 Address,
        _In_ ULONG Width,
        _Out_writes_bytes_(Width) PVOID Buffer);

    RTVM_STATUS
    (RTVMAPI *MemoryWrite)(
        _In_ PRTVM_DEVICE Device,
        _In_ ULONG64 Address,
        _In_ ULONG Width,
        _In_reads_bytes_(Width) const VOID *Buffer);

    /* The time asked for through SetTimer has passed */
    VOID (RTVMAPI *Timer)(_In_ PRTVM_DEVICE Device);

    /*
     * The operator did something. Only a device standing in for a thing they can
     * touch offers this, and the manager passes each event to all of them.
     */
    RTVM_STATUS
    (RTVMAPI *Input)(
        _In_ PRTVM_DEVICE Device,
        _In_ RTVM_INPUT_KIND Kind,
        _In_ ULONG Value);
} RTVM_DEVICE_VTABLE, *PRTVM_DEVICE_VTABLE;

/*
 * One piece of hardware. The module allocates it, fills in the table and its
 * own context, and the manager fills in the rest before anything is called.
 */
struct _RTVM_DEVICE
{
    ULONG Size;
    const RTVM_DEVICE_VTABLE *Vtable;

    /* Set by the manager, not the module */
    const RTVM_HOST_INTERFACE *Host;

    /* The module's own, which the manager never looks inside */
    PVOID DeviceContext;

    /* What this instance is called in the log and on the command line */
    CHAR Name[32];
};

/*
 * A kind of device the module offers. Parameters is whatever followed the kind
 * name on the command line, or NULL, and means whatever the kind decides.
 */
typedef struct _RTVM_DEVICE_CLASS
{
    PCSTR Name;
    PCSTR Description;

    RTVM_STATUS
    (RTVMAPI *Create)(
        _In_ const RTVM_HOST_INTERFACE *Host,
        _In_opt_ PCSTR Parameters,
        _Outptr_ PRTVM_DEVICE *Device);
} RTVM_DEVICE_CLASS, *PRTVM_DEVICE_CLASS;

/* What a module says it is, and what it has */
typedef struct _RTVM_DEVICE_MODULE
{
    ULONG Size;
    ULONG AbiVersion;
    PCSTR ModuleName;
    PCSTR Description;
    ULONG ClassCount;
    const RTVM_DEVICE_CLASS *Classes;
} RTVM_DEVICE_MODULE, *PRTVM_DEVICE_MODULE;

/*
 * The one thing a module exports. It is given the version the manager speaks
 * and answers with what it has, or NULL if the two cannot agree.
 *
 * The manager only ever reaches it by name, so a module marks the definition
 * with RTVM_DEVICE_EXPORT and needs neither a definition file nor an import
 * library for it.
 */
typedef const RTVM_DEVICE_MODULE *
(RTVMAPI *PFN_RTVM_DEVICE_MODULE_ENTRY)(
    _In_ ULONG HostAbiVersion);

#define RTVM_DEVICE_MODULE_ENTRY_NAME "RtvmDeviceModuleEntry"

#define RTVM_DEVICE_EXPORT __declspec(dllexport)

#ifdef __cplusplus
}
#endif
