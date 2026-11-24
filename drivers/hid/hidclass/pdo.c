/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Collection PDOs and their Plug and Play handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static
SIZE_T
NTAPI
HidClassMultiSzLength(
    _In_ PCWSTR MultiSz)
{
    PCWSTR Current = MultiSz;

    while (*Current)
        Current += wcslen(Current) + 1;

    return (SIZE_T)(Current - MultiSz);
}

/**
 * @brief
 * Turns the IDs the bus gave the device into collection IDs. Each ID loses
 * its enumerator, gets HID\ in front and &ColNN behind on devices with more
 * than one top level collection.
 */
static
PWSTR
NTAPI
HidClassSubstituteBusNames(
    _In_ PCWSTR Ids,
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    BOOLEAN AddCollection = (Fdo->DeviceDesc.CollectionDescLength > 1);
    PCWSTR Source;
    PWSTR Result;
    PWSTR Target;
    SIZE_T Count = 1;
    SIZE_T Left;

    for (Source = Ids; *Source; Source += wcslen(Source) + 1)
        Count += wcslen(Source) + 4 + 1 + (AddCollection ? 6 : 0);

    Result = ExAllocatePoolZero(PagedPool, Count * sizeof(WCHAR), HIDCLASS_TAG);
    if (!Result)
        return NULL;

    Target = Result;
    Left = Count;
    for (Source = Ids; *Source; Source += wcslen(Source) + 1)
    {
        PCWSTR Separator = wcschr(Source, L'\\');
        PCWSTR Rest = Separator ? Separator + 1 : Source;
        PWSTR End;

        if (AddCollection)
            RtlStringCchPrintfExW(Target, Left, &End, NULL, 0, L"HID\\%ls&Col%02X", Rest, Pdo->CollectionNumber);
        else
            RtlStringCchPrintfExW(Target, Left, &End, NULL, 0, L"HID\\%ls", Rest);

        Left -= (End - Target) + 1;
        Target = End + 1;
    }
    *Target = UNICODE_NULL;

    return Result;
}

/**
 * @brief
 * Builds the IDs hidclass derives from the vendor, the product and the top
 * level usage of the collection. They follow the bus IDs in the hardware ID
 * list, most specific first, ending with HID_DEVICE.
 */
static
PWSTR
NTAPI
HidClassBuildUsageIds(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PHIDP_COLLECTION_DESC CollectionDesc = &Fdo->DeviceDesc.CollectionDesc[Pdo->CollectionIndex];
    USHORT VendorId = Fdo->Attributes.VendorID;
    USHORT ProductId = Fdo->Attributes.ProductID;
    USAGE UsagePage = CollectionDesc->UsagePage;
    USAGE Usage = CollectionDesc->Usage;
    PCWSTR SystemId = NULL;
    const SIZE_T Count = 512;
    PWSTR Result;
    PWSTR Target;
    PWSTR End;
    SIZE_T Left;

    if (UsagePage == HID_USAGE_PAGE_GENERIC)
    {
        switch (Usage)
        {
            case HID_USAGE_GENERIC_POINTER:
            case HID_USAGE_GENERIC_MOUSE:
                SystemId = L"HID_DEVICE_SYSTEM_MOUSE";
                break;

            case HID_USAGE_GENERIC_JOYSTICK:
            case HID_USAGE_GENERIC_GAMEPAD:
                SystemId = L"HID_DEVICE_SYSTEM_GAME";
                break;

            case HID_USAGE_GENERIC_KEYBOARD:
            case HID_USAGE_GENERIC_KEYPAD:
                SystemId = L"HID_DEVICE_SYSTEM_KEYBOARD";
                break;

            case HID_USAGE_GENERIC_SYSTEM_CTL:
                SystemId = L"HID_DEVICE_SYSTEM_CONTROL";
                break;
        }
    }
    else if (UsagePage == HID_USAGE_PAGE_CONSUMER)
    {
        SystemId = L"HID_DEVICE_SYSTEM_CONSUMER";
    }

    if (UsagePage >= 0xFF00)
        SystemId = L"HID_DEVICE_UPR:FF00-FFFF";

    Result = ExAllocatePoolZero(PagedPool, Count * sizeof(WCHAR), HIDCLASS_TAG);
    if (!Result)
        return NULL;

    Target = Result;
    Left = Count;

#define HIDCLASS_APPEND_ID(...) \
    do \
    { \
        RtlStringCchPrintfExW(Target, Left, &End, NULL, 0, __VA_ARGS__); \
        Left -= (End - Target) + 1; \
        Target = End + 1; \
    } while (0)

    if (VendorId)
    {
        if (Fdo->ComboHardwareIdV2 && ProductId)
        {
            if (Fdo->DeviceDesc.CollectionDescLength > 1)
            {
                HIDCLASS_APPEND_ID(L"HID\\VID_%04X&PID_%04X&Col%02X&UP:%04X_U:%04X",
                                   VendorId, ProductId, Pdo->CollectionNumber, UsagePage, Usage);
            }
            else
            {
                HIDCLASS_APPEND_ID(L"HID\\VID_%04X&PID_%04X&UP:%04X_U:%04X",
                                   VendorId, ProductId, UsagePage, Usage);
            }
        }

        HIDCLASS_APPEND_ID(L"HID\\VID_%04X&UP:%04X_U:%04X", VendorId, UsagePage, Usage);
    }

    /* Vendor defined pages list the range after the exact usage */
    if (SystemId && UsagePage < 0xFF00)
        HIDCLASS_APPEND_ID(L"%ls", SystemId);

    HIDCLASS_APPEND_ID(L"HID_DEVICE_UP:%04X_U:%04X", UsagePage, Usage);

    if (SystemId && UsagePage >= 0xFF00)
        HIDCLASS_APPEND_ID(L"%ls", SystemId);

    HIDCLASS_APPEND_ID(L"HID_DEVICE");

#undef HIDCLASS_APPEND_ID

    *Target = UNICODE_NULL;
    return Result;
}

/**
 * @brief
 * Asks the minidriver, and so the bus below it, for the IDs of the device.
 */
static
NTSTATUS
NTAPI
HidClassQueryBusIds(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp,
    _Out_ PWSTR *Ids)
{
    NTSTATUS Status;

    *Ids = NULL;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    Status = HidClassCallMinidriverSynchronous(Fdo, Irp);
    if (!NT_SUCCESS(Status))
        return Status;

    *Ids = (PWSTR)Irp->IoStatus.Information;
    Irp->IoStatus.Information = 0;
    return *Ids ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

static
NTSTATUS
NTAPI
HidClassPdoQueryId(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PWSTR BusIds;
    PWSTR Copy;
    PWSTR Result;
    PWSTR UsageIds;
    SIZE_T Length;
    SIZE_T UsageLength;
    ULONG Index;
    NTSTATUS Status;

    switch (IoStack->Parameters.QueryId.IdType)
    {
        case BusQueryDeviceID:
            Status = HidClassQueryBusIds(Fdo, Irp, &BusIds);
            if (!NT_SUCCESS(Status))
                return Status;

            /* A single string, extended to a list so it can be rewritten like one */
            Length = wcslen(BusIds);
            Copy = ExAllocatePoolZero(PagedPool, (Length + 2) * sizeof(WCHAR), HIDCLASS_TAG);
            if (!Copy)
            {
                ExFreePool(BusIds);
                return STATUS_DEVICE_DATA_ERROR;
            }
            RtlCopyMemory(Copy, BusIds, Length * sizeof(WCHAR));
            ExFreePool(BusIds);

            Result = HidClassSubstituteBusNames(Copy, Fdo, Pdo);
            ExFreePoolWithTag(Copy, HIDCLASS_TAG);
            if (!Result)
                return STATUS_DEVICE_DATA_ERROR;

            Irp->IoStatus.Information = (ULONG_PTR)Result;
            return STATUS_SUCCESS;

        case BusQueryHardwareIDs:
            Status = HidClassQueryBusIds(Fdo, Irp, &BusIds);
            if (!NT_SUCCESS(Status))
                return Status;

            Copy = HidClassSubstituteBusNames(BusIds, Fdo, Pdo);
            ExFreePool(BusIds);
            if (!Copy)
                return STATUS_INSUFFICIENT_RESOURCES;

            UsageIds = HidClassBuildUsageIds(Fdo, Pdo);
            if (!UsageIds)
            {
                ExFreePoolWithTag(Copy, HIDCLASS_TAG);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            Length = HidClassMultiSzLength(Copy);
            UsageLength = HidClassMultiSzLength(UsageIds);
            Result = ExAllocatePoolZero(PagedPool, (Length + UsageLength + 1) * sizeof(WCHAR), HIDCLASS_TAG);
            if (Result)
            {
                RtlCopyMemory(Result, Copy, Length * sizeof(WCHAR));
                RtlCopyMemory(Result + Length, UsageIds, (UsageLength + 1) * sizeof(WCHAR));
                Irp->IoStatus.Information = (ULONG_PTR)Result;
                Status = STATUS_SUCCESS;
            }
            else
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
            }

            ExFreePoolWithTag(UsageIds, HIDCLASS_TAG);
            ExFreePoolWithTag(Copy, HIDCLASS_TAG);
            return Status;

        case BusQueryCompatibleIDs:
            Result = ExAllocatePoolZero(PagedPool, 2 * sizeof(WCHAR), HIDCLASS_TAG);
            if (!Result)
                return STATUS_INSUFFICIENT_RESOURCES;

            Irp->IoStatus.Information = (ULONG_PTR)Result;
            return STATUS_SUCCESS;

        case BusQueryInstanceID:
            Result = ExAllocatePoolZero(PagedPool, 5 * sizeof(WCHAR), HIDCLASS_TAG);
            if (!Result)
                return STATUS_INSUFFICIENT_RESOURCES;

            RtlCopyMemory(Result, L"0000", 5 * sizeof(WCHAR));
            if (Fdo->ClientPdoRelations)
            {
                for (Index = 0; Index < Fdo->ClientPdoRelations->Count; Index++)
                {
                    if (Fdo->ClientPdoRelations->Objects[Index] == Pdo->DeviceObject)
                    {
                        RtlStringCchPrintfW(Result, 5, L"%04x", Index);
                        break;
                    }
                }
            }

            Irp->IoStatus.Information = (ULONG_PTR)Result;
            return STATUS_SUCCESS;

        default:
            return Irp->IoStatus.Status;
    }
}

static
VOID
NTAPI
HidClassSetInterfaceProperties(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDP_COLLECTION_DESC CollectionDesc)
{
    PUNICODE_STRING Name = &Collection->InterfaceName;
    DEVPROP_BOOLEAN ReadOnly;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Pdo);

    ReadOnly = (CollectionDesc->OutputLength || CollectionDesc->FeatureLength) ? DEVPROP_FALSE : DEVPROP_TRUE;

    Status = IoSetDeviceInterfacePropertyData(Name, &DEVPKEY_DeviceInterface_HID_UsagePage, 0, 0,
                                              DEVPROP_TYPE_UINT16, sizeof(USHORT), &CollectionDesc->UsagePage);
    if (NT_SUCCESS(Status))
        Status = IoSetDeviceInterfacePropertyData(Name, &DEVPKEY_DeviceInterface_HID_UsageId, 0, 0,
                                                  DEVPROP_TYPE_UINT16, sizeof(USHORT), &CollectionDesc->Usage);
    if (NT_SUCCESS(Status))
        Status = IoSetDeviceInterfacePropertyData(Name, &DEVPKEY_DeviceInterface_HID_IsReadOnly, 0, 0,
                                                  DEVPROP_TYPE_BOOLEAN, sizeof(ReadOnly), &ReadOnly);
    if (NT_SUCCESS(Status))
        Status = IoSetDeviceInterfacePropertyData(Name, &DEVPKEY_DeviceInterface_HID_VendorId, 0, 0,
                                                  DEVPROP_TYPE_UINT16, sizeof(USHORT), &Collection->Information.VendorID);
    if (NT_SUCCESS(Status))
        Status = IoSetDeviceInterfacePropertyData(Name, &DEVPKEY_DeviceInterface_HID_ProductId, 0, 0,
                                                  DEVPROP_TYPE_UINT16, sizeof(USHORT), &Collection->Information.ProductID);
    if (NT_SUCCESS(Status))
        Status = IoSetDeviceInterfacePropertyData(Name, &DEVPKEY_DeviceInterface_HID_VersionNumber, 0, 0,
                                                  DEVPROP_TYPE_UINT16, sizeof(USHORT), &Collection->Information.VersionNumber);

    if (!NT_SUCCESS(Status))
        DPRINT("HIDCLASS: interface properties not set %lx\n", Status);
}

/**
 * @brief
 * Exposes the collection as a HID interface, keyboards with the KBD
 * reference string, plus a system button interface when the collection
 * reports power buttons.
 */
static
NTSTATUS
NTAPI
HidClassPdoRegisterInterface(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PHIDP_COLLECTION_DESC CollectionDesc;
    PHIDCLASS_COLLECTION Collection;
    UNICODE_STRING Reference;
    ULONG Caps = 0;
    NTSTATUS Status;

    Collection = HidClassFindCollection(Pdo->Fdo, Pdo->CollectionNumber);
    CollectionDesc = HidClassFindCollectionDesc(Pdo->Fdo, Pdo->CollectionNumber);
    if (!Collection || !CollectionDesc)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    RtlInitUnicodeString(&Reference, L"KBD");
    Status = IoRegisterDeviceInterface(Pdo->DeviceObject,
                                       &GUID_DEVINTERFACE_HID,
                                       (Collection->Kind == HidClassCollectionKeyboard) ? &Reference : NULL,
                                       &Collection->InterfaceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: interface registration failed %lx\n", Status);
        return Status;
    }

    HidClassSetInterfaceProperties(Pdo, Collection, CollectionDesc);

    Status = IoSetDeviceInterfaceState(&Collection->InterfaceName, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!NT_SUCCESS(HidP_SysPowerCaps(Collection->PreparsedData, &Caps)) || !Caps)
        return STATUS_SUCCESS;

    /* Portable device controls report buttons without being a system button */
    if (CollectionDesc->UsagePage == HID_USAGE_PAGE_GENERIC && CollectionDesc->Usage == 0x0D)
        return STATUS_SUCCESS;

    Status = IoRegisterDeviceInterface(Pdo->DeviceObject,
                                       &GUID_DEVICE_SYS_BUTTON,
                                       NULL,
                                       &Collection->SysButtonInterfaceName);
    if (NT_SUCCESS(Status))
        Status = IoSetDeviceInterfaceState(&Collection->SysButtonInterfaceName, TRUE);

    return Status;
}

static
VOID
NTAPI
HidClassPdoUnregisterInterface(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PHIDCLASS_COLLECTION Collection = HidClassFindCollection(Pdo->Fdo, Pdo->CollectionNumber);

    if (!Collection)
        return;

    if (Collection->InterfaceName.Buffer)
    {
        IoSetDeviceInterfaceState(&Collection->InterfaceName, FALSE);
        RtlFreeUnicodeString(&Collection->InterfaceName);
    }

    if (Collection->SysButtonInterfaceName.Buffer)
    {
        IoSetDeviceInterfaceState(&Collection->SysButtonInterfaceName, FALSE);
        RtlFreeUnicodeString(&Collection->SysButtonInterfaceName);
    }
}

/**
 * @brief
 * Fails the system button event request a collection holds.
 */
static
VOID
NTAPI
HidClassFreePowerEventIrp(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PHIDCLASS_COLLECTION Collection = HidClassFindCollection(Pdo->Fdo, Pdo->CollectionNumber);
    PIRP Irp = NULL;
    KIRQL OldIrql;

    if (!Collection)
        return;

    KeAcquireSpinLock(&Collection->PowerEventLock, &OldIrql);
    if (Collection->PowerEventIrp)
    {
        if (IoSetCancelRoutine(Collection->PowerEventIrp, NULL))
            Irp = Collection->PowerEventIrp;
        Collection->PowerEventIrp = NULL;
    }
    KeReleaseSpinLock(&Collection->PowerEventLock, OldIrql);

    if (!Irp)
        return;

    *(PULONG)Irp->AssociatedIrp.SystemBuffer = 0;
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
NTSTATUS
NTAPI
HidClassCreateCollectionPdo(
    _In_ PHIDCLASS_DRIVER_EXTENSION DriverExtension,
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG Index)
{
    PHIDCLASS_DEVICE_EXTENSION FdoExtension = HidClassExtensionFromFdo(Fdo);
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension;
    PHIDCLASS_COLLECTION Collection = &Fdo->Collections[Index];
    PHIDCLASS_PDO_EXTENSION Pdo;
    PDEVICE_OBJECT DeviceObject;
    ULONG ExtensionSize;
    NTSTATUS Status;
    KIRQL OldIrql;

    ExtensionSize = HIDCLASS_EXTENSION_SIZE + DriverExtension->DeviceExtensionSize;
    Status = IoCreateDevice(DriverExtension->DriverObject,
                            ExtensionSize,
                            NULL,
                            FILE_DEVICE_UNKNOWN,
                            FILE_AUTOGENERATED_DEVICE_NAME,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    ObReferenceObject(DeviceObject);
    DeviceObject->StackSize = Fdo->DeviceObject->StackSize + 1;

    DeviceExtension = DeviceObject->DeviceExtension;
    RtlZeroMemory(DeviceExtension, ExtensionSize);

    /* The minidriver view stays the one of the FDO */
    DeviceExtension->Hid = FdoExtension->Hid;
    DeviceExtension->IsClientPdo = TRUE;
    DeviceExtension->Signature = HIDCLASS_EXTENSION_SIGNATURE;

    Pdo = &DeviceExtension->Pdo;
    Pdo->CollectionNumber = Collection->CollectionNumber;
    Pdo->CollectionIndex = Index;
    Pdo->DeviceObject = DeviceObject;
    Pdo->FdoDeviceObject = Fdo->DeviceObject;
    Pdo->Fdo = Fdo;
    Pdo->State = HidClassPdoUninitialized;
    Pdo->PreviousState = HidClassPdoUninitialized;
    Pdo->SystemPowerState = Fdo->SystemPowerState;
    Pdo->DevicePowerState = Fdo->DevicePowerState;
    Pdo->IsKeyboardOrMouse = (Collection->Kind != HidClassCollectionOther);
    IoInitializeRemoveLock(&Pdo->RemoveLock, HIDCLASS_TAG, 0, 0);
    KeInitializeSpinLock(&Pdo->RemoteWakeLock);

    Fdo->ClientPdoRelations->Objects[Index] = DeviceObject;

    KeAcquireSpinLock(&Fdo->ClientPdoLock, &OldIrql);
    Fdo->ClientPdos[Index] = Pdo;
    KeReleaseSpinLock(&Fdo->ClientPdoLock, OldIrql);

    DeviceObject->Flags |= DO_POWER_PAGABLE | DO_DIRECT_IO;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates one PDO per top level collection.
 */
NTSTATUS
NTAPI
HidClassCreateCollectionPdos(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;
    ULONG Count = Fdo->DeviceDesc.CollectionDescLength;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;

    DriverExtension = HidClassReferenceDriver(Fdo->DriverExtension->DriverObject);
    if (!DriverExtension || Count == 0)
    {
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    if (!Fdo->Collections)
    {
        Status = STATUS_INVALID_DEVICE_STATE;
        goto Exit;
    }

    if (!Fdo->ClientPdoRelations)
    {
        Fdo->ClientPdoRelations = ExAllocatePoolZero(NonPagedPool,
                                                     FIELD_OFFSET(DEVICE_RELATIONS, Objects[Count]),
                                                     HIDCLASS_TAG);
    }

    if (!Fdo->ClientPdos)
        Fdo->ClientPdos = ExAllocatePoolZero(NonPagedPool, Count * sizeof(*Fdo->ClientPdos), HIDCLASS_TAG);

    if (!Fdo->ClientPdoRelations || !Fdo->ClientPdos)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Failed;
    }

    Fdo->ClientPdoRelations->Count = Count;
    for (Index = 0; Index < Count; Index++)
    {
        Status = HidClassCreateCollectionPdo(DriverExtension, Fdo, Index);
        if (!NT_SUCCESS(Status))
            goto Failed;
    }

    goto Exit;

Failed:
    if (Fdo->ClientPdos)
    {
        ExFreePoolWithTag(Fdo->ClientPdos, HIDCLASS_TAG);
        Fdo->ClientPdos = NULL;
    }

    if (Fdo->ClientPdoRelations)
    {
        ExFreePoolWithTag(Fdo->ClientPdoRelations, HIDCLASS_TAG);
        Fdo->ClientPdoRelations = NULL;
    }

Exit:
    if (DriverExtension)
        HidClassDereferenceDriver(DriverExtension->DriverObject);

    return Status;
}

static
NTSTATUS
NTAPI
HidClassPdoStartDevice(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PHIDCLASS_COLLECTION Collection;
    ULONG Value;
    ULONG Index;

    if (Pdo->State == HidClassPdoUninitialized)
        Pdo->State = HidClassPdoStarting;

    Collection = HidClassFindCollection(Fdo, Pdo->CollectionNumber);
    if (!Collection)
        return STATUS_DEVICE_DATA_ERROR;

    /* Pens and touch screens only talk to the session at the console */
    Pdo->SessionSecurity = (Collection->UsagePage == HID_USAGE_PAGE_DIGITIZER &&
                            Collection->Usage >= 1 && Collection->Usage <= 4);
    if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo->DeviceObject, L"SessionSecurityEnabled", &Value)))
        Pdo->SessionSecurity = (Value != 0);

    if (Collection->Information.Polled && Fdo->ClientPdoRelations)
    {
        for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
            HidClassStartPolling(Fdo, &Fdo->Collections[Index]);
    }

    Pdo->State = HidClassPdoStarted;
    HidClassPdoRegisterInterface(Pdo);

    if (HidClassPdoCanWake(Pdo))
    {
        if (NT_SUCCESS(HidClassQueryDeviceRegistryValue(Pdo->DeviceObject, L"RemoteWakeEnabled", &Value)))
            Pdo->RemoteWakeEnabled = (Value != 0);

        HidClassInitializePdoWmi(Pdo);
        HidClassRequestRemoteWake(Pdo);
    }

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
HidClassPdoRemoveDevice(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo)
{
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PHIDCLASS_COLLECTION Collection;
    KIRQL OldIrql;

    Pdo->State = HidClassPdoUninitialized;

    if (Pdo->StatusChange)
        Pdo->StatusChange(Pdo->StatusChangeContext, DeviceObjectRemoved);

    if (Pdo->WmiRegistered)
    {
        IoWMIRegistrationControl(Pdo->DeviceObject, WMIREG_ACTION_DEREGISTER);
        Pdo->WmiRegistered = FALSE;
    }

    HidClassCancelRemoteWake(Pdo);

    Collection = HidClassFindCollection(Fdo, Pdo->CollectionNumber);
    if (Collection)
    {
        if (Collection->Information.Polled)
            HidClassStopPolling(Collection, TRUE);

        HidClassCompletePendingReads(Pdo);
    }

    /* A PDO still reported to PnP lives on until the FDO goes */
    if (Fdo->RelationsReported)
        return;

    KeAcquireSpinLock(&Fdo->ClientPdoLock, &OldIrql);
    if (Fdo->ClientPdos)
        Fdo->ClientPdos[Pdo->CollectionIndex] = NULL;
    KeReleaseSpinLock(&Fdo->ClientPdoLock, OldIrql);

    ObDereferenceObject(Pdo->DeviceObject);
    IoDeleteDevice(Pdo->DeviceObject);
}

static
NTSTATUS
NTAPI
HidClassPdoQueryInterface(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    const GUID *InterfaceType = IoStack->Parameters.QueryInterface.InterfaceType;

    if (IsEqualGUID(InterfaceType, &GUID_HID_INTERFACE_NOTIFY))
    {
        PHID_INTERFACE_NOTIFY_PNP Notify = (PHID_INTERFACE_NOTIFY_PNP)IoStack->Parameters.QueryInterface.Interface;

        if (Notify->Size != sizeof(*Notify) || !Notify->Version || !Notify->StatusChangeFn)
            return Irp->IoStatus.Status;

        Pdo->StatusChange = Notify->StatusChangeFn;
        Pdo->StatusChangeContext = Notify->CallbackContext;
        return STATUS_SUCCESS;
    }

    if (IsEqualGUID(InterfaceType, &GUID_HID_INTERFACE_HIDPARSE))
    {
        PHID_INTERFACE_HIDPARSE Parse = (PHID_INTERFACE_HIDPARSE)IoStack->Parameters.QueryInterface.Interface;

        if (Parse->Size != sizeof(*Parse) || !Parse->Version)
            return Irp->IoStatus.Status;

        Parse->HidpGetCaps = HidP_GetCaps;
        return STATUS_SUCCESS;
    }

    return Irp->IoStatus.Status;
}

static
NTSTATUS
NTAPI
HidClassPdoQueryCapabilities(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PDEVICE_CAPABILITIES Capabilities;

    Capabilities = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceCapabilities.Capabilities;
    if (!Capabilities)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    *Capabilities = Pdo->Fdo->Capabilities;
    Capabilities->LockSupported = FALSE;
    Capabilities->EjectSupported = FALSE;
    Capabilities->Removable = FALSE;
    Capabilities->DockDevice = FALSE;
    Capabilities->UniqueID = FALSE;
    Capabilities->SilentInstall = TRUE;
    Capabilities->Address = Pdo->CollectionNumber;

    /* Keyboards and mice need their class driver, anything else may run raw */
    Capabilities->RawDeviceOK = !Pdo->IsKeyboardOrMouse;
    Capabilities->SurpriseRemovalOK = TRUE;

    Irp->IoStatus.Information = (ULONG_PTR)Capabilities;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidClassPdoPnp(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FDO_EXTENSION Fdo = Pdo->Fdo;
    PDEVICE_RELATIONS Relations;
    PPNP_BUS_INFORMATION BusInformation;
    UCHAR MinorFunction = IoStack->MinorFunction;
    NTSTATUS Status = STATUS_SUCCESS;

    switch (MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = HidClassPdoStartDevice(Pdo);
            if (NT_SUCCESS(Status) && Pdo->StatusChange)
                Pdo->StatusChange(Pdo->StatusChangeContext, DeviceObjectStarted);
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            Pdo->PreviousState = Pdo->State;
            Pdo->State = HidClassPdoRemovePending;

            if (Pdo->PreviousState == HidClassPdoStarted || Pdo->PreviousState == HidClassPdoStopped)
            {
                if (Pdo->PreviousState == HidClassPdoStarted)
                    HidClassPdoUnregisterInterface(Pdo);

                HidClassCompletePendingReads(Pdo);
                HidClassFreePowerEventIrp(Pdo);
                HidClassCompleteDelayedIrpsForPdo(Pdo, STATUS_NO_SUCH_DEVICE);
                HidClassCancelResetNotifications(Fdo, Pdo);
            }

            if (MinorFunction == IRP_MN_REMOVE_DEVICE)
                HidClassPdoRemoveDevice(Pdo);
            break;

        case IRP_MN_CANCEL_REMOVE_DEVICE:
            if (Pdo->State == HidClassPdoRemovePending)
            {
                Pdo->State = Pdo->PreviousState;
                if (Pdo->State == HidClassPdoStarted)
                    HidClassPdoRegisterInterface(Pdo);
            }
            break;

        case IRP_MN_STOP_DEVICE:
            if (Pdo->State != HidClassPdoUninitialized)
            {
                HidClassPdoUnregisterInterface(Pdo);
                HidClassFreePowerEventIrp(Pdo);
                Pdo->State = HidClassPdoStopped;
                if (Pdo->StatusChange)
                    Pdo->StatusChange(Pdo->StatusChangeContext, DeviceObjectStopped);
            }
            break;

        case IRP_MN_QUERY_STOP_DEVICE:
            Pdo->PreviousState = Pdo->State;
            Pdo->State = HidClassPdoStopPending;
            break;

        case IRP_MN_CANCEL_STOP_DEVICE:
            if (Pdo->State == HidClassPdoStopPending)
                Pdo->State = Pdo->PreviousState;
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (IoStack->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
            {
                Status = Irp->IoStatus.Status;
                break;
            }

            Relations = ExAllocatePoolZero(PagedPool, sizeof(*Relations), HIDCLASS_TAG);
            if (!Relations)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            ObReferenceObject(Pdo->DeviceObject);
            Relations->Count = 1;
            Relations->Objects[0] = Pdo->DeviceObject;
            Irp->IoStatus.Information = (ULONG_PTR)Relations;
            break;

        case IRP_MN_QUERY_INTERFACE:
            Status = HidClassPdoQueryInterface(Pdo, Irp);
            break;

        case IRP_MN_QUERY_CAPABILITIES:
            Status = HidClassPdoQueryCapabilities(Pdo, Irp);
            break;

        case IRP_MN_QUERY_ID:
            Status = HidClassPdoQueryId(Pdo, Irp);
            break;

        case IRP_MN_QUERY_PNP_DEVICE_STATE:
            if (Pdo->State == HidClassPdoStopPending)
                Irp->IoStatus.Information |= PNP_DEVICE_FAILED;
            else if (Pdo->State == HidClassPdoRemovePending)
                Irp->IoStatus.Information |= PNP_DEVICE_DISABLED;
            break;

        case IRP_MN_QUERY_BUS_INFORMATION:
            BusInformation = ExAllocatePoolZero(PagedPool, sizeof(*BusInformation), HIDCLASS_TAG);
            if (!BusInformation)
            {
                Irp->IoStatus.Information = 0;
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            BusInformation->BusTypeGuid = GUID_BUS_TYPE_HID;
            BusInformation->LegacyBusType = PNPBus;
            BusInformation->BusNumber = Fdo->HidId;
            Irp->IoStatus.Information = (ULONG_PTR)BusInformation;
            break;

        case IRP_MN_DEVICE_ENUMERATED:
            break;

        default:
            Status = Irp->IoStatus.Status;
            break;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
