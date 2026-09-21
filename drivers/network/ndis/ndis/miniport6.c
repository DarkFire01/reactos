/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6.x miniport driver registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"
#include <ndiswdf.h>

NTSTATUS
NTAPI
NdisIAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject);

NTSTATUS
NTAPI
NdisGenericIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp);

/*
 * Every one of these is mandatory for a 6.0 miniport. Registering without one
 * would only move the fault to the first call that needs it, which is a far
 * worse place to find out.
 */
static
BOOLEAN
NTAPI
NdispValidateCharacteristics6(
    _In_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics)
{
    if (Characteristics->InitializeHandlerEx == NULL ||
        Characteristics->HaltHandlerEx == NULL ||
        Characteristics->PauseHandler == NULL ||
        Characteristics->RestartHandler == NULL ||
        Characteristics->OidRequestHandler == NULL ||
        Characteristics->SendNetBufferListsHandler == NULL ||
        Characteristics->ReturnNetBufferListsHandler == NULL ||
        Characteristics->CancelSendHandler == NULL ||
        Characteristics->DevicePnPEventNotifyHandler == NULL ||
        Characteristics->ShutdownHandlerEx == NULL ||
        Characteristics->CancelOidRequestHandler == NULL)
    {
        return FALSE;
    }

    return TRUE;
}

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMRegisterMiniportDriver(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath,
    NDIS_HANDLE MiniportDriverContext,
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    PNDIS_HANDLE NdisMiniportDriverHandle)
{
    PNDIS_M_DRIVER_BLOCK Miniport;
    PNDIS_M_DRIVER_BLOCK *MiniportPtr;
    NTSTATUS Status;
    ULONG i;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    *NdisMiniportDriverHandle = NULL;

    if (MiniportDriverCharacteristics->Header.Type !=
        NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Bad characteristics object type.\n"));
        return NDIS_STATUS_BAD_CHARACTERISTICS;
    }

    if (MiniportDriverCharacteristics->Header.Size <
        NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Bad characteristics length.\n"));
        return NDIS_STATUS_BAD_CHARACTERISTICS;
    }

    /* This entry point is 6.x only. A 5.x driver belongs on NdisMRegisterMiniport. */
    if (MiniportDriverCharacteristics->MajorNdisVersion != 6)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Bad miniport characteristics version %u.%u\n",
                                  MiniportDriverCharacteristics->MajorNdisVersion,
                                  MiniportDriverCharacteristics->MinorNdisVersion));
        return NDIS_STATUS_BAD_VERSION;
    }

    if (!NdispValidateCharacteristics6(MiniportDriverCharacteristics))
    {
        NDIS_DbgPrint(MIN_TRACE, ("Missing a mandatory 6.x handler.\n"));
        return NDIS_STATUS_BAD_CHARACTERISTICS;
    }

    NDIS_DbgPrint(MID_TRACE, ("Registering an NDIS %u.%u miniport driver\n",
                              MiniportDriverCharacteristics->MajorNdisVersion,
                              MiniportDriverCharacteristics->MinorNdisVersion));

    /* The caller's path need not outlive the call, so the block carries a copy */
    Miniport = ExAllocatePoolWithTag(NonPagedPool,
                                     sizeof(*Miniport) + RegistryPath->Length + sizeof(UNICODE_NULL),
                                     NDIS_TAG);
    if (Miniport == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
        return NDIS_STATUS_RESOURCES;
    }

    RtlZeroMemory(Miniport, sizeof(*Miniport) + RegistryPath->Length + sizeof(UNICODE_NULL));

    Miniport->ServiceKeyPath.Buffer = (PWCH)(Miniport + 1);
    Miniport->ServiceKeyPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
    RtlCopyUnicodeString(&Miniport->ServiceKeyPath, RegistryPath);

    KeInitializeSpinLock(&Miniport->Lock);
    InitializeListHead(&Miniport->DeviceList);
    Miniport->DriverObject = DriverObject;
    Miniport->RegistryPath = &Miniport->ServiceKeyPath;
    Miniport->MiniportDriverContext = MiniportDriverContext;
    Miniport->Ndis6Driver = TRUE;
    Miniport->UnhookedCharacteristics = &Miniport->Characteristics6;

    RtlCopyMemory(&Miniport->Characteristics6,
                  MiniportDriverCharacteristics,
                  min(MiniportDriverCharacteristics->Header.Size,
                      sizeof(Miniport->Characteristics6)));

    /*
     * The PnP path finds the driver block through the driver object extension,
     * the same way the 5.x registration does, so AddDevice needs no 6.x
     * specific lookup.
     */
    Status = IoAllocateDriverObjectExtension(DriverObject,
                                             (PVOID)'NMID',
                                             sizeof(PNDIS_M_DRIVER_BLOCK),
                                             (PVOID *)&MiniportPtr);
    if (Status == STATUS_OBJECT_NAME_COLLISION)
    {
        /*
         * A driver object only gets one extension per client id, so a driver
         * that registers again after deregistering reuses the one it already
         * has instead of failing.
         */
        MiniportPtr = IoGetDriverObjectExtension(DriverObject, (PVOID)'NMID');
        Status = (MiniportPtr != NULL) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(Status))
    {
        NDIS_DbgPrint(MIN_TRACE, ("Can't allocate driver object extension.\n"));
        ExFreePoolWithTag(Miniport, NDIS_TAG);
        return NDIS_STATUS_RESOURCES;
    }

    *MiniportPtr = Miniport;

    /* WDF owns the dispatch table of a driver a class extension registers */
    if (!(MiniportDriverCharacteristics->Flags & NDIS_WDF_PNP_POWER_HANDLING))
    {
        for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
            DriverObject->MajorFunction[i] = NdisGenericIrpHandler;

        DriverObject->DriverExtension->AddDevice = NdisIAddDevice;
    }

    ExInterlockedInsertTailList(&MiniportListHead, &Miniport->ListEntry, &MiniportListLock);

    /* The handle is out before SetOptions, which may look the driver up by it */
    *NdisMiniportDriverHandle = Miniport;

    /* Optional handlers can only be registered from here, against the new handle */
    if (Miniport->Characteristics6.SetOptionsHandler != NULL)
    {
        NDIS_STATUS NdisStatus;

        NdisStatus = Miniport->Characteristics6.SetOptionsHandler(Miniport, MiniportDriverContext);
        if (NdisStatus != NDIS_STATUS_SUCCESS)
        {
            NDIS_DbgPrint(MIN_TRACE, ("MiniportSetOptions failed (0x%x).\n", NdisStatus));
            *NdisMiniportDriverHandle = NULL;
            ExInterlockedRemoveEntryList(&Miniport->ListEntry, &MiniportListLock);
            *MiniportPtr = NULL;
            ExFreePoolWithTag(Miniport, NDIS_TAG);
            return NdisStatus;
        }
    }

    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
NdisMDeregisterMiniportDriver(
    NDIS_HANDLE NdisMiniportDriverHandle)
{
    PNDIS_M_DRIVER_BLOCK Miniport = (PNDIS_M_DRIVER_BLOCK)NdisMiniportDriverHandle;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    ExInterlockedRemoveEntryList(&Miniport->ListEntry, &MiniportListLock);

    /* A hooked driver's own handlers were copied aside */
    if (Miniport->UnhookedCharacteristics != &Miniport->Characteristics6)
        ExFreePoolWithTag(Miniport->UnhookedCharacteristics, NDIS_TAG);

    ExFreePoolWithTag(Miniport, NDIS_TAG);
}

/* Attributes */

/*
 * The core keeps the NDIS 5 attribute flags, so the 6.x registration flags are
 * mapped onto them. A 6.x miniport is always deserialized.
 */
static
ULONG
Mini6TranslateAttributeFlags(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ ULONG AttributeFlags)
{
    ULONG Flags = NDIS_ATTRIBUTE_DESERIALIZE | NDIS_ATTRIBUTE_USES_SAFE_BUFFER_APIS;

    if (Adapter->NdisMiniportBlock.DriverHandle->Characteristics6.Flags & NDIS_INTERMEDIATE_DRIVER)
        Flags |= NDIS_ATTRIBUTE_INTERMEDIATE_DRIVER;
    if (AttributeFlags & NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK)
        Flags |= NDIS_ATTRIBUTE_SURPRISE_REMOVE_OK;
    if (AttributeFlags & NDIS_MINIPORT_ATTRIBUTES_NOT_CO_NDIS)
        Flags |= NDIS_ATTRIBUTE_NOT_CO_NDIS;
    if (AttributeFlags & NDIS_MINIPORT_ATTRIBUTES_DO_NOT_BIND_TO_ALL_CO)
        Flags |= NDIS_ATTRIBUTE_DO_NOT_BIND_TO_ALL_CO;
    if (AttributeFlags & NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND)
        Flags |= NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND;
    if (AttributeFlags & NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER)
        Flags |= NDIS_ATTRIBUTE_BUS_MASTER;

    return Flags;
}

/**
 * @brief
 * Records one set of attributes a 6.x miniport describes itself with.
 *
 * @param[in] NdisMiniportHandle
 * The adapter handle given to MiniportInitializeEx.
 *
 * @param[in] MiniportAttributes
 * The attributes, dispatched on their header type.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or the reason the attributes were refused.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMSetMiniportAttributes(
    NDIS_HANDLE NdisMiniportHandle,
    PNDIS_MINIPORT_ADAPTER_ATTRIBUTES MiniportAttributes)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisMiniportHandle;

    NDIS_DbgPrint(MAX_TRACE, ("Called, type 0x%x.\n", MiniportAttributes->Header.Type));

    switch (MiniportAttributes->Header.Type)
    {
        case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES:
        {
            PNDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES Registration =
                &MiniportAttributes->RegistrationAttributes;

            NdisMSetAttributesEx(NdisMiniportHandle,
                                 Registration->MiniportAdapterContext,
                                 Registration->CheckForHangTimeInSeconds,
                                 Mini6TranslateAttributeFlags(Adapter, Registration->AttributeFlags),
                                 Registration->InterfaceType);
            return NDIS_STATUS_SUCCESS;
        }

        case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES:
            return CoreSetGeneralAttributes(Adapter, &MiniportAttributes->GeneralAttributes);

        case NDIS_OBJECT_TYPE_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES:
            Adapter->Core.AddDeviceContext = MiniportAttributes->AddDeviceRegistrationAttributes.MiniportAddDeviceContext;
            return NDIS_STATUS_SUCCESS;

        case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES:
        case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES:
        case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_HARDWARE_ASSIST_ATTRIBUTES:
        case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_NDK_ATTRIBUTES:
            /* No protocol here asks for these features, so nothing is recorded */
            NDIS_DbgPrint(MID_TRACE, ("Attributes type 0x%x accepted and not used.\n",
                                      MiniportAttributes->Header.Type));
            return NDIS_STATUS_SUCCESS;

        default:
            NDIS_DbgPrint(MIN_TRACE, ("Unknown attributes type 0x%x.\n",
                                      MiniportAttributes->Header.Type));
            return STATUS_INVALID_PARAMETER;
    }
}
