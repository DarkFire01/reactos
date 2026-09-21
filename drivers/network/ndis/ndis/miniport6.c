/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6.x miniport driver registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

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

    Miniport = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Miniport), NDIS_TAG);
    if (Miniport == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
        return NDIS_STATUS_RESOURCES;
    }

    RtlZeroMemory(Miniport, sizeof(*Miniport));

    KeInitializeSpinLock(&Miniport->Lock);
    InitializeListHead(&Miniport->DeviceList);
    Miniport->DriverObject = DriverObject;
    Miniport->RegistryPath = RegistryPath;
    Miniport->MiniportDriverContext = MiniportDriverContext;
    Miniport->Ndis6Driver = TRUE;

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

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = NdisGenericIrpHandler;

    DriverObject->DriverExtension->AddDevice = NdisIAddDevice;

    ExInterlockedInsertTailList(&MiniportListHead, &Miniport->ListEntry, &MiniportListLock);

    *NdisMiniportDriverHandle = Miniport;

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
    ExFreePoolWithTag(Miniport, NDIS_TAG);
}

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

            /*
             * The 6.x registration attributes carry what NdisMSetAttributesEx
             * took as separate arguments, so the 5.x path does the work.
             */
            NdisMSetAttributesEx(NdisMiniportHandle,
                                 Registration->MiniportAdapterContext,
                                 Registration->CheckForHangTimeInSeconds,
                                 Registration->AttributeFlags,
                                 Registration->InterfaceType);

            return NDIS_STATUS_SUCCESS;
        }

        case NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES:
            /* Accepted and ignored until the general attributes are described. */
            NDIS_DbgPrint(MID_TRACE, ("General attributes not handled yet.\n"));
            return NDIS_STATUS_SUCCESS;

        default:
            NDIS_DbgPrint(MIN_TRACE, ("Unsupported attributes type 0x%x.\n",
                                      MiniportAttributes->Header.Type));
            UNREFERENCED_PARAMETER(Adapter);
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}
