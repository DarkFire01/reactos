/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device property store (IoGet/SetDevicePropertyData)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A per-device-node, in-memory property store keyed by DEVPROPKEY. This is the
 * channel the interrupt-assignment path uses to publish INTERRUPT_CONNECTION_DATA
 * (under INTERRUPT_CONNECTION_DATA_PKEY) for the bus driver (pci.sys) and for the
 * kernel's own IoConnectInterruptEx to consume. It is deliberately volatile:
 * Windows does not persist the connection-data key either. Registry persistence
 * for non-volatile keys is not implemented.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES *********************************************************************/

typedef struct _IOP_DEVICE_PROPERTY
{
    LIST_ENTRY ListEntry;
    DEVPROPKEY Key;
    DEVPROPTYPE Type;
    ULONG Size;
    _Field_size_bytes_(Size) UCHAR Data[ANYSIZE_ARRAY];
} IOP_DEVICE_PROPERTY, *PIOP_DEVICE_PROPERTY;

/* GLOBALS *******************************************************************/

/* Protects every device node's DeviceProperties list. Zero-initialised, which is
   a valid unowned push lock (== ExInitializePushLock). */
static EX_PUSH_LOCK IopDevicePropertyLock;

/* PRIVATE FUNCTIONS *********************************************************/

/**
 * @brief
 * Returns the stored property with the given key on a device node,
 * if any. Caller holds IopDevicePropertyLock.
 *
 * @param[in] DeviceNode
 * The device node whose DeviceProperties list is searched.
 *
 * @param[in] Key
 * The DEVPROPKEY to look up.
 *
 * @return
 * Returns the stored property, or NULL if the key has no value on
 * this node.
 */
static
PIOP_DEVICE_PROPERTY
IopFindDeviceProperty(
    _In_ PDEVICE_NODE DeviceNode,
    _In_ const DEVPROPKEY *Key)
{
    PLIST_ENTRY Entry;

    for (Entry = DeviceNode->DeviceProperties.Flink;
         Entry != &DeviceNode->DeviceProperties;
         Entry = Entry->Flink)
    {
        PIOP_DEVICE_PROPERTY Property =
            CONTAINING_RECORD(Entry, IOP_DEVICE_PROPERTY, ListEntry);

        /* DEVPROPKEY is a GUID plus a ULONG pid, tightly packed (no padding) */
        if (RtlEqualMemory(&Property->Key, Key, sizeof(DEVPROPKEY)))
            return Property;
    }

    return NULL;
}

/**
 * @brief
 * Releases every property stored on a device node.
 *
 * @param[in] DeviceNode
 * The device node being destroyed.
 *
 * @remarks
 * Called from IopFreeDeviceNode. The node is no longer reachable,
 * but the lock is taken anyway to stay correct against any
 * concurrent get/set; the node's list is reparented onto a private
 * head under the lock, then freed outside it.
 */
VOID
IopFreeDeviceProperties(
    _In_ PDEVICE_NODE DeviceNode)
{
    LIST_ENTRY Dead;
    PLIST_ENTRY Entry;

    InitializeListHead(&Dead);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&IopDevicePropertyLock);

    if (!IsListEmpty(&DeviceNode->DeviceProperties))
    {
        Dead = DeviceNode->DeviceProperties;
        Dead.Flink->Blink = &Dead;
        Dead.Blink->Flink = &Dead;
        InitializeListHead(&DeviceNode->DeviceProperties);
    }

    ExReleasePushLockExclusive(&IopDevicePropertyLock);
    KeLeaveCriticalRegion();

    while (!IsListEmpty(&Dead))
    {
        Entry = RemoveHeadList(&Dead);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, IOP_DEVICE_PROPERTY, ListEntry),
                          TAG_IO_DEVPROP);
    }
}

/* PUBLIC FUNCTIONS **********************************************************/

/**
 * @brief
 * Stores (or replaces, or deletes) a property value on the device
 * node of a PDO, keyed by DEVPROPKEY.
 *
 * @param[in] Pdo
 * The physical device object whose device node stores the value.
 *
 * @param[in] PropertyKey
 * The DEVPROPKEY identifying the property.
 *
 * @param[in] Lcid
 * Must be LOCALE_NEUTRAL (0): binary/scalar properties are
 * locale-neutral.
 *
 * @param[in] Flags
 * Unreferenced (registry persistence is not implemented; every
 * stored key is volatile).
 *
 * @param[in] Type
 * The DEVPROP_TYPE_* of the value.
 *
 * @param[in] Size
 * The value size in bytes; 0 (or a NULL Data) deletes the key.
 *
 * @param[in] Data
 * The value bytes to copy in, or NULL to delete the key.
 *
 * @return
 * Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER on a NULL
 * Pdo/PropertyKey or non-neutral locale,
 * STATUS_INVALID_DEVICE_REQUEST for a PDO without a device node,
 * or STATUS_INSUFFICIENT_RESOURCES.
 *
 * @remarks
 * The replacement allocation is made outside the property lock;
 * setting always replaces any previous value for the key.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
NTAPI
IoSetDevicePropertyData(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _In_ ULONG Flags,
    _In_ DEVPROPTYPE Type,
    _In_ ULONG Size,
    _In_reads_bytes_opt_(Size) PVOID Data)
{
    PDEVICE_NODE DeviceNode;
    PIOP_DEVICE_PROPERTY Existing;
    PIOP_DEVICE_PROPERTY NewProperty = NULL;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Flags);

    if ((Pdo == NULL) || (PropertyKey == NULL))
        return STATUS_INVALID_PARAMETER;

    /* Binary/scalar properties must be locale-neutral (LOCALE_NEUTRAL == 0) */
    if (Lcid != 0)
        return STATUS_INVALID_PARAMETER;

    DeviceNode = IopGetDeviceNode(Pdo);
    if (DeviceNode == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;

    /* A non-empty value; allocate its replacement outside the lock */
    if ((Size != 0) && (Data != NULL))
    {
        NewProperty = ExAllocatePoolWithTag(NonPagedPool,
                                            FIELD_OFFSET(IOP_DEVICE_PROPERTY, Data) + Size,
                                            TAG_IO_DEVPROP);
        if (NewProperty == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        NewProperty->Key = *PropertyKey;
        NewProperty->Type = Type;
        NewProperty->Size = Size;
        RtlCopyMemory(NewProperty->Data, Data, Size);
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&IopDevicePropertyLock);

    /* Setting a property always replaces any previous value for the key */
    Existing = IopFindDeviceProperty(DeviceNode, PropertyKey);
    if (Existing != NULL)
        RemoveEntryList(&Existing->ListEntry);

    if (NewProperty != NULL)
        InsertTailList(&DeviceNode->DeviceProperties, &NewProperty->ListEntry);

    ExReleasePushLockExclusive(&IopDevicePropertyLock);
    KeLeaveCriticalRegion();

    if (Existing != NULL)
        ExFreePoolWithTag(Existing, TAG_IO_DEVPROP);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads a property value stored on the device node of a PDO,
 * keyed by DEVPROPKEY, with the two-call size convention.
 *
 * @param[in] Pdo
 * The physical device object whose device node holds the value.
 *
 * @param[in] PropertyKey
 * The DEVPROPKEY identifying the property.
 *
 * @param[in] Lcid
 * Must be LOCALE_NEUTRAL.
 *
 * @param[in] Flags
 * Reserved, unreferenced.
 *
 * @param[in] Size
 * The caller's buffer size in bytes.
 *
 * @param[out] Data
 * The caller's buffer, or NULL when probing for the size.
 *
 * @param[out] RequiredSize
 * Receives the stored value's size (0 when not found).
 *
 * @param[out] Type
 * Receives the stored DEVPROP_TYPE_* (DEVPROP_TYPE_EMPTY when not
 * found).
 *
 * @return
 * Returns STATUS_SUCCESS with the value copied out,
 * STATUS_BUFFER_TOO_SMALL with *RequiredSize set when the buffer
 * cannot hold it, STATUS_NOT_FOUND when the key has no value,
 * STATUS_INVALID_PARAMETER on a NULL Pdo/PropertyKey/RequiredSize/
 * Type or non-neutral locale, or STATUS_INVALID_DEVICE_REQUEST for
 * a PDO without a device node.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
NTAPI
IoGetDevicePropertyData(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _Reserved_ ULONG Flags,
    _In_ ULONG Size,
    _Out_writes_bytes_opt_(Size) PVOID Data,
    _Out_ PULONG RequiredSize,
    _Out_ PDEVPROPTYPE Type)
{
    PDEVICE_NODE DeviceNode;
    PIOP_DEVICE_PROPERTY Property;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Flags);

    if ((Pdo == NULL) || (PropertyKey == NULL) ||
        (RequiredSize == NULL) || (Type == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((Lcid != 0) && (Lcid != LOCALE_NEUTRAL))
        return STATUS_INVALID_PARAMETER;

    DeviceNode = IopGetDeviceNode(Pdo);
    if (DeviceNode == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&IopDevicePropertyLock);

    Property = IopFindDeviceProperty(DeviceNode, PropertyKey);
    if (Property == NULL)
    {
        *RequiredSize = 0;
        *Type = DEVPROP_TYPE_EMPTY;
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    *Type = Property->Type;
    *RequiredSize = Property->Size;

    /* Two-call convention: report the size when the buffer is absent/too small */
    if (Size < Property->Size)
    {
        Status = STATUS_BUFFER_TOO_SMALL;
        goto Exit;
    }

    if (Data == NULL)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    RtlCopyMemory(Data, Property->Data, Property->Size);
    Status = STATUS_SUCCESS;

Exit:
    ExReleasePushLockShared(&IopDevicePropertyLock);
    KeLeaveCriticalRegion();
    return Status;
}

/* EOF */
