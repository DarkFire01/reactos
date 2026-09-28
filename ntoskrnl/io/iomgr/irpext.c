/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What an IRP carries beyond its stack locations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A driver that wants an IRP to carry more than its stack locations hold asks
 * for the bigger size up front and initializes the packet with it. The room
 * then sits past the last stack location and costs no allocation of its own.
 * A driver that did not ask still gets what it sets, out of pool.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE DEFINITIONS ********************************************************/

#define TAG_IRP_EXTENSION 'EprI'

/* A device object saying every IRP bound for it wants the extra room */
#define IOP_DEVICE_WANTS_EXTENSION 0x08000000

/* Stands for a device that is not known yet, and is answered for as if it did */
#define IOP_ANY_DEVICE ((PDEVICE_OBJECT)~(ULONG_PTR)0)

/* Set on an IRP whose extension came out of pool and has to go back */
#define IOP_EXTENSION_FROM_POOL 0x40

/* What the room can hold, one bit each */
#define IOP_EXTENSION_ACTIVITY_ID 0
#define IOP_EXTENSION_TRACKING    2

/* What a caller of IoPropagateIrpExtension asks to be carried over */
#define IOP_PROPAGATE_TRACKING    0x01
#define IOP_PROPAGATE_ACTIVITY_ID 0x02

typedef struct _IOP_IRP_EXTENSION
{
    ULONG Present;
    ULONG TrackingValue;
    GUID ActivityId;
} IOP_IRP_EXTENSION, *PIOP_IRP_EXTENSION;

/* PRIVATE FUNCTIONS **********************************************************/

static
BOOLEAN
IopDeviceWantsIrpExtension(
    _In_opt_ PDEVICE_OBJECT DeviceObject)
{
    if (DeviceObject == NULL)
        return FALSE;

    if (DeviceObject == IOP_ANY_DEVICE)
        return TRUE;

    return (DeviceObject->Flags & IOP_DEVICE_WANTS_EXTENSION) != 0;
}

/**
 * @brief
 * Finds the room an IRP keeps one kind of thing in, making room if there is
 * none.
 *
 * @return
 * The extension with that kind marked present, or NULL if there was no pool
 * for one.
 */
static
PIOP_IRP_EXTENSION
IopAllocateIrpExtension(
    _Inout_ PIRP Irp,
    _In_ ULONG Kind)
{
    PIOP_IRP_EXTENSION Extension = Irp->Tail.Overlay.IrpExtension;

    if (Extension == NULL)
    {
        Extension = ExAllocatePoolZero(NonPagedPool,
                                       sizeof(*Extension),
                                       TAG_IRP_EXTENSION);
        if (Extension == NULL)
            return NULL;

        Irp->AllocationFlags |= IOP_EXTENSION_FROM_POOL;
        Irp->Tail.Overlay.IrpExtension = Extension;
    }

    Extension->Present |= (1 << Kind);

    return Extension;
}

/**
 * @brief
 * Drops one kind of thing from an IRP, and the room itself once the last kind
 * is gone.
 */
static
VOID
IopReleaseIrpExtension(
    _Inout_ PIRP Irp,
    _In_ ULONG Kind)
{
    PIOP_IRP_EXTENSION Extension = Irp->Tail.Overlay.IrpExtension;

    if (Extension == NULL)
        return;

    Extension->Present &= ~(1 << Kind);

    if (Extension->Present == 0)
        IopFreeIrpExtension(Irp);
}

/**
 * @brief
 * Drops everything an IRP carries past its stack locations.
 *
 * @remarks
 * Room that came with the packet goes with the packet, so only pool is given
 * back here.
 */
VOID
NTAPI
IopFreeIrpExtension(
    _Inout_ PIRP Irp)
{
    if (Irp->AllocationFlags & IOP_EXTENSION_FROM_POOL)
    {
        ExFreePoolWithTag(Irp->Tail.Overlay.IrpExtension, TAG_IRP_EXTENSION);
        Irp->AllocationFlags &= ~IOP_EXTENSION_FROM_POOL;
    }

    Irp->Tail.Overlay.IrpExtension = NULL;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
USHORT
NTAPI
IoSizeOfIrpEx(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ CCHAR StackSize)
{
    /* The room is measured in stack locations so that the packet stays aligned */
    if (IopDeviceWantsIrpExtension(DeviceObject))
        StackSize += 2;

    return IoSizeOfIrp(StackSize);
}

/*
 * @implemented
 */
VOID
NTAPI
IoInitializeIrpEx(
    _Out_ PIRP Irp,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ USHORT PacketSize,
    _In_ CCHAR StackSize)
{
    IoInitializeIrp(Irp, PacketSize, StackSize);

    /* What IoSizeOfIrpEx counted starts one past the last stack location */
    if (IopDeviceWantsIrpExtension(DeviceObject))
        Irp->Tail.Overlay.IrpExtension = Irp->Tail.Overlay.CurrentStackLocation;
}

/*
 * @implemented
 */
VOID
NTAPI
IoCleanupIrp(
    _Inout_ PIRP Irp)
{
    /* Only the driver that made this one gets to say it is done with it */
    ASSERT(Irp->Type == IO_TYPE_IRP);

    Irp->Type = 0;

    IopFreeIrpExtension(Irp);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoGetActivityIdIrp(
    _In_ PIRP Irp,
    _Out_ LPGUID Guid)
{
    PIOP_IRP_EXTENSION Extension = Irp->Tail.Overlay.IrpExtension;

    if (Extension == NULL ||
        !(Extension->Present & (1 << IOP_EXTENSION_ACTIVITY_ID)))
    {
        return STATUS_NOT_FOUND;
    }

    *Guid = Extension->ActivityId;

    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoSetActivityIdIrp(
    _Inout_ PIRP Irp,
    _In_opt_ LPCGUID Guid)
{
    PIOP_IRP_EXTENSION Extension;
    LPCGUID Current;

    Extension = IopAllocateIrpExtension(Irp, IOP_EXTENSION_ACTIVITY_ID);
    if (Extension == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Guid != NULL)
    {
        Extension->ActivityId = *Guid;
        return STATUS_SUCCESS;
    }

    /* Naming nothing means whatever the thread behind the request is doing */
    if (PsGetCurrentThread() != Irp->Tail.Overlay.Thread)
    {
        IopReleaseIrpExtension(Irp, IOP_EXTENSION_ACTIVITY_ID);
        return STATUS_NOT_SUPPORTED;
    }

    Current = IoGetActivityIdThread();
    if (Current == NULL)
    {
        IopReleaseIrpExtension(Irp, IOP_EXTENSION_ACTIVITY_ID);
        return STATUS_NOT_FOUND;
    }

    Extension->ActivityId = *Current;

    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoPropagateIrpExtension(
    _In_ PIRP SourceIrp,
    _Inout_ PIRP TargetIrp,
    _In_ ULONG Flags)
{
    PIOP_IRP_EXTENSION Source = SourceIrp->Tail.Overlay.IrpExtension;
    PIOP_IRP_EXTENSION Target;

    if (Source == NULL)
        return STATUS_SUCCESS;

    if ((Flags & IOP_PROPAGATE_ACTIVITY_ID) &&
        (Source->Present & (1 << IOP_EXTENSION_ACTIVITY_ID)))
    {
        Target = IopAllocateIrpExtension(TargetIrp, IOP_EXTENSION_ACTIVITY_ID);
        if (Target == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Target->ActivityId = Source->ActivityId;
    }

    if ((Flags & IOP_PROPAGATE_TRACKING) &&
        (Source->Present & (1 << IOP_EXTENSION_TRACKING)))
    {
        Target = IopAllocateIrpExtension(TargetIrp, IOP_EXTENSION_TRACKING);
        if (Target == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Target->TrackingValue = Source->TrackingValue;
    }

    /*
     * The remaining flags name things this system never puts on an IRP, so
     * there is nothing of them to carry over.
     */
    return STATUS_SUCCESS;
}

/* EOF */
