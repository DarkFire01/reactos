/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for NDIS 6.x miniport driver registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kmt_test.h>

#define NDIS60 1
#include <ndis.h>

/* A complete but inert 6.0 miniport. None of these run in this test. */
static MINIPORT_INITIALIZE TestInitializeEx;
static MINIPORT_HALT TestHaltEx;
static MINIPORT_PAUSE TestPause;
static MINIPORT_RESTART TestRestart;
static MINIPORT_OID_REQUEST TestOidRequest;
static MINIPORT_SEND_NET_BUFFER_LISTS TestSendNetBufferLists;
static MINIPORT_RETURN_NET_BUFFER_LISTS TestReturnNetBufferLists;
static MINIPORT_CANCEL_SEND TestCancelSend;
static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TestDevicePnPEventNotify;
static MINIPORT_SHUTDOWN TestShutdownEx;
static MINIPORT_CANCEL_OID_REQUEST TestCancelOidRequest;

static
NDIS_STATUS
NTAPI
TestInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    UNREFERENCED_PARAMETER(NdisMiniportHandle);
    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(MiniportInitParameters);
    return NDIS_STATUS_SUCCESS;
}

static VOID NTAPI TestHaltEx(_In_ NDIS_HANDLE C, _In_ NDIS_HALT_ACTION A)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(A); }

static NDIS_STATUS NTAPI TestPause(_In_ NDIS_HANDLE C, _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS P)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(P); return NDIS_STATUS_SUCCESS; }

static NDIS_STATUS NTAPI TestRestart(_In_ NDIS_HANDLE C, _In_ PNDIS_MINIPORT_RESTART_PARAMETERS P)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(P); return NDIS_STATUS_SUCCESS; }

static NDIS_STATUS NTAPI TestOidRequest(_In_ NDIS_HANDLE C, _In_ struct _NDIS_OID_REQUEST *R)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(R); return NDIS_STATUS_SUCCESS; }

static VOID NTAPI TestSendNetBufferLists(_In_ NDIS_HANDLE C, _In_ PNET_BUFFER_LIST N,
                                         _In_ NDIS_PORT_NUMBER P, _In_ ULONG F)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(N);
  UNREFERENCED_PARAMETER(P); UNREFERENCED_PARAMETER(F); }

static VOID NTAPI TestReturnNetBufferLists(_In_ NDIS_HANDLE C, _In_ PNET_BUFFER_LIST N, _In_ ULONG F)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(N); UNREFERENCED_PARAMETER(F); }

static VOID NTAPI TestCancelSend(_In_ NDIS_HANDLE C, _In_ PVOID I)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(I); }

static VOID NTAPI TestDevicePnPEventNotify(_In_ NDIS_HANDLE C, _In_ struct _NET_DEVICE_PNP_EVENT *E)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(E); }

static VOID NTAPI TestShutdownEx(_In_ NDIS_HANDLE C, _In_ NDIS_SHUTDOWN_ACTION A)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(A); }

static VOID NTAPI TestCancelOidRequest(_In_ NDIS_HANDLE C, _In_ PVOID I)
{ UNREFERENCED_PARAMETER(C); UNREFERENCED_PARAMETER(I); }

static
VOID
NTAPI
FillCharacteristics(
    _Out_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics)
{
    RtlZeroMemory(Characteristics, sizeof(*Characteristics));

    Characteristics->Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    Characteristics->Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1;
    Characteristics->Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1;

    Characteristics->MajorNdisVersion = 6;
    Characteristics->MinorNdisVersion = 0;
    Characteristics->MajorDriverVersion = 1;
    Characteristics->MinorDriverVersion = 0;

    Characteristics->InitializeHandlerEx = TestInitializeEx;
    Characteristics->HaltHandlerEx = TestHaltEx;
    Characteristics->PauseHandler = TestPause;
    Characteristics->RestartHandler = TestRestart;
    Characteristics->OidRequestHandler = TestOidRequest;
    Characteristics->SendNetBufferListsHandler = TestSendNetBufferLists;
    Characteristics->ReturnNetBufferListsHandler = TestReturnNetBufferLists;
    Characteristics->CancelSendHandler = TestCancelSend;
    Characteristics->DevicePnPEventNotifyHandler = TestDevicePnPEventNotify;
    Characteristics->ShutdownHandlerEx = TestShutdownEx;
    Characteristics->CancelOidRequestHandler = TestCancelOidRequest;
}

VOID
NTAPI
TestMiniport6(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics;
    PDRIVER_DISPATCH SavedMajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    PDRIVER_ADD_DEVICE SavedAddDevice;
    NDIS_HANDLE DriverHandle;
    NDIS_STATUS Status;
    ULONG i;

    /*
     * Registering takes over the driver object's whole dispatch table and its
     * AddDevice, which is right for a real miniport and fatal for a kmtest
     * driver that still has to answer its own IRPs. Put them back afterwards.
     */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        SavedMajorFunction[i] = DriverObject->MajorFunction[i];
    SavedAddDevice = DriverObject->DriverExtension->AddDevice;

    /* A complete 6.0 driver registers. */
    FillCharacteristics(&Characteristics);
    DriverHandle = NULL;
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    ok(DriverHandle != NULL, "no driver handle on success\n");

    if (Status == NDIS_STATUS_SUCCESS)
        NdisMDeregisterMiniportDriver(DriverHandle);

    /* An NDIS 5 version on the 6.x entry point is rejected. */
    FillCharacteristics(&Characteristics);
    Characteristics.MajorNdisVersion = 5;
    Characteristics.MinorNdisVersion = 1;
    DriverHandle = (NDIS_HANDLE)(ULONG_PTR)0xBAD;
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_BAD_VERSION);
    ok_eq_pointer(DriverHandle, NULL);

    /* A wrong object type is rejected before anything else is looked at. */
    FillCharacteristics(&Characteristics);
    Characteristics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_BAD_CHARACTERISTICS);

    /* A short characteristics block is rejected. */
    FillCharacteristics(&Characteristics);
    Characteristics.Header.Size = sizeof(NDIS_OBJECT_HEADER);
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_BAD_CHARACTERISTICS);

    /*
     * Each mandatory handler is mandatory. Dropping one at a time is the only
     * way to catch a validation list that silently lost an entry.
     */
    FillCharacteristics(&Characteristics);
    Characteristics.InitializeHandlerEx = NULL;
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_BAD_CHARACTERISTICS);

    FillCharacteristics(&Characteristics);
    Characteristics.SendNetBufferListsHandler = NULL;
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_BAD_CHARACTERISTICS);

    FillCharacteristics(&Characteristics);
    Characteristics.CancelOidRequestHandler = NULL;
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_BAD_CHARACTERISTICS);

    /* Registering again after all that must still work. */
    FillCharacteristics(&Characteristics);
    DriverHandle = NULL;
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL,
                                         &Characteristics, &DriverHandle);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    if (Status == NDIS_STATUS_SUCCESS)
        NdisMDeregisterMiniportDriver(DriverHandle);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = SavedMajorFunction[i];
    DriverObject->DriverExtension->AddDevice = SavedAddDevice;
}
