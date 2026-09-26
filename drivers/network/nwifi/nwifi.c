/*
 * PROJECT:     ReactOS Native WiFi filter
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Light-weight filter over native 802.11 miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "nwifi.h"
#include <dot11wdi.h>

#define NDEBUG
#include <debug.h>

/* What a send of ours carries, to finish the list it was built from */
typedef struct _NWIFI_SEND
{
    PNET_BUFFER_LIST Parent;
    LONG Pending;
    NDIS_STATUS Status;
} NWIFI_SEND, *PNWIFI_SEND;

#define NWIFI_SEND_OF(_Nbl) (*(PNWIFI_SEND *)&NWIFI_FRAME_CONTEXT_OF(_Nbl)->Owner)

/* Every NDIS_STATUS_DOT11_* sits in this range */
#define NWIFI_IS_DOT11_STATUS(_Status) (((ULONG)(_Status) & 0xFFFF0000) == 0x40030000)

NDIS_HANDLE NwifiDriverHandle;

static LIST_ENTRY NwifiModules;
static KSPIN_LOCK NwifiModulesLock;

static DRIVER_UNLOAD NwifiUnload;
static FILTER_ATTACH NwifiAttach;
static FILTER_DETACH NwifiDetach;
static FILTER_RESTART NwifiRestart;
static FILTER_PAUSE NwifiPause;
static FILTER_SEND_NET_BUFFER_LISTS NwifiSend;
static FILTER_SEND_NET_BUFFER_LISTS_COMPLETE NwifiSendComplete;
static FILTER_RECEIVE_NET_BUFFER_LISTS NwifiReceive;
static FILTER_RETURN_NET_BUFFER_LISTS NwifiReturn;
static FILTER_STATUS NwifiStatus;
FILTER_OID_REQUEST_COMPLETE NwifiOidRequestComplete;

/* Modules the control device can reach */

/**
 * @brief
 * Finds the module of an interface for the control device and references it.
 *
 * @param[in] InterfaceGuid
 * The interface.
 *
 * @return
 * The module, or NULL when the filter is not attached to it.
 */
PNWIFI_MODULE
NTAPI
NwifiReferenceModule(
    _In_ const GUID *InterfaceGuid)
{
    PNWIFI_MODULE Found = NULL;
    PNWIFI_MODULE Module;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&NwifiModulesLock, &OldIrql);

    for (Entry = NwifiModules.Flink; Entry != &NwifiModules; Entry = Entry->Flink)
    {
        Module = CONTAINING_RECORD(Entry, NWIFI_MODULE, ListEntry);
        if (IsEqualGUID(&Module->InterfaceGuid, InterfaceGuid))
        {
            InterlockedIncrement(&Module->References);
            Found = Module;
            break;
        }
    }

    KeReleaseSpinLock(&NwifiModulesLock, OldIrql);
    return Found;
}

/**
 * @brief
 * Drops a reference taken by NwifiReferenceModule.
 */
VOID
NTAPI
NwifiDereferenceModule(
    _In_ PNWIFI_MODULE Module)
{
    if (InterlockedDecrement(&Module->References) == 0)
        KeSetEvent(&Module->Unreferenced, IO_NO_INCREMENT, FALSE);
}

/* Our own lists */

static
VOID
NwifiFreeFrame(
    _In_ PNWIFI_MODULE Module,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    NdisFreeNetBufferList(NetBufferList);

    if (InterlockedDecrement(&Module->Outstanding) == 0)
        KeSetEvent(&Module->Drained, IO_NO_INCREMENT, FALSE);
}

static
VOID
NwifiCountFrame(
    _In_ PNWIFI_MODULE Module)
{
    if (InterlockedIncrement(&Module->Outstanding) == 1)
        KeClearEvent(&Module->Drained);
}

static
ULONG
NwifiSendCompleteFlags(VOID)
{
    return (KeGetCurrentIrql() == DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;
}

/* Attach and detach */

static
NTSTATUS
NwifiGuidFromMiniportName(
    _In_ PNDIS_STRING MiniportName,
    _Out_ GUID *InterfaceGuid)
{
    UNICODE_STRING GuidString;
    USHORT i;

    /* The name is \DEVICE\{GUID} */
    for (i = 0; i < MiniportName->Length / sizeof(WCHAR); i++)
    {
        if (MiniportName->Buffer[i] == L'{')
            break;
    }

    GuidString.Buffer = MiniportName->Buffer + i;
    GuidString.Length = MiniportName->Length - i * sizeof(WCHAR);
    GuidString.MaximumLength = GuidString.Length;

    return RtlGUIDFromString(&GuidString, InterfaceGuid);
}

static
NDIS_STATUS
NTAPI
NwifiAttach(
    _In_ NDIS_HANDLE NdisFilterHandle,
    _In_ NDIS_HANDLE FilterDriverContext,
    _In_ PNDIS_FILTER_ATTACH_PARAMETERS AttachParameters)
{
    NET_BUFFER_LIST_POOL_PARAMETERS PoolParameters;
    NDIS_FILTER_ATTRIBUTES Attributes;
    PNWIFI_MODULE Module;
    NDIS_STATUS Status;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(FilterDriverContext);

    if (AttachParameters->MiniportMediaType != NdisMediumNative802_11)
        return NDIS_STATUS_NOT_SUPPORTED;

    Module = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Module), NWIFI_TAG);
    if (Module == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Module, sizeof(*Module));
    Module->FilterHandle = NdisFilterHandle;
    Module->References = 1;
    KeInitializeEvent(&Module->Unreferenced, NotificationEvent, FALSE);
    KeInitializeEvent(&Module->Drained, NotificationEvent, TRUE);
    KeInitializeSpinLock(&Module->Lock);

    if (!NT_SUCCESS(NwifiGuidFromMiniportName(AttachParameters->BaseMiniportName, &Module->InterfaceGuid)))
    {
        ExFreePoolWithTag(Module, NWIFI_TAG);
        return NDIS_STATUS_FAILURE;
    }

    Module->Indications = ExAllocatePoolWithTag(NonPagedPool,
                                                NWIFI_INDICATION_SLOTS * sizeof(NWIFI_QUEUED_INDICATION),
                                                NWIFI_TAG);

    RtlZeroMemory(&PoolParameters, sizeof(PoolParameters));
    PoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    PoolParameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    PoolParameters.fAllocateNetBuffer = TRUE;
    PoolParameters.ContextSize = NWIFI_FRAME_CONTEXT_SIZE;
    PoolParameters.PoolTag = NWIFI_TAG;
    PoolParameters.DataSize = NWIFI_FRAME_SIZE;
    Module->NblPool = NdisAllocateNetBufferListPool(NdisFilterHandle, &PoolParameters);

    if (Module->Indications == NULL || Module->NblPool == NULL)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Failed;
    }

    RtlZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Header.Type = NDIS_OBJECT_TYPE_FILTER_ATTRIBUTES;
    Attributes.Header.Revision = NDIS_FILTER_ATTRIBUTES_REVISION_1;
    Attributes.Header.Size = NDIS_SIZEOF_FILTER_ATTRIBUTES_REVISION_1;

    Status = NdisFSetAttributes(NdisFilterHandle, Module, &Attributes);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Failed;

    KeAcquireSpinLock(&NwifiModulesLock, &OldIrql);
    InsertTailList(&NwifiModules, &Module->ListEntry);
    KeReleaseSpinLock(&NwifiModulesLock, OldIrql);

    DPRINT("Attached to %wZ\n", AttachParameters->BaseMiniportName);
    return NDIS_STATUS_SUCCESS;

Failed:
    if (Module->NblPool != NULL)
        NdisFreeNetBufferListPool(Module->NblPool);
    if (Module->Indications != NULL)
        ExFreePoolWithTag(Module->Indications, NWIFI_TAG);
    ExFreePoolWithTag(Module, NWIFI_TAG);
    return Status;
}

static
VOID
NTAPI
NwifiDetach(
    _In_ NDIS_HANDLE FilterModuleContext)
{
    PNWIFI_MODULE Module = FilterModuleContext;
    KIRQL OldIrql;

    KeAcquireSpinLock(&NwifiModulesLock, &OldIrql);
    RemoveEntryList(&Module->ListEntry);
    KeReleaseSpinLock(&NwifiModulesLock, OldIrql);

    /* Control device requests still using it, and OID requests of ours, finish first */
    NwifiDereferenceModule(Module);
    KeWaitForSingleObject(&Module->Unreferenced, Executive, KernelMode, FALSE, NULL);

    NdisFreeNetBufferListPool(Module->NblPool);
    ExFreePoolWithTag(Module->Indications, NWIFI_TAG);
    ExFreePoolWithTag(Module, NWIFI_TAG);
}

/* Pause and restart */

static
NDIS_STATUS
NTAPI
NwifiRestart(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNDIS_FILTER_RESTART_PARAMETERS RestartParameters)
{
    PNWIFI_MODULE Module = FilterModuleContext;

    UNREFERENCED_PARAMETER(RestartParameters);

    Module->Running = TRUE;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NTAPI
NwifiPause(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNDIS_FILTER_PAUSE_PARAMETERS PauseParameters)
{
    PNWIFI_MODULE Module = FilterModuleContext;

    UNREFERENCED_PARAMETER(PauseParameters);

    /* Every list of ours comes back before the pause is done */
    Module->Running = FALSE;
    KeWaitForSingleObject(&Module->Drained, Executive, KernelMode, FALSE, NULL);

    return NDIS_STATUS_SUCCESS;
}

/* Send */

static
VOID
NwifiFinishSend(
    _In_ PNWIFI_SEND Send,
    _Inout_ PNET_BUFFER_LIST *Done)
{
    NET_BUFFER_LIST_STATUS(Send->Parent) = Send->Status;
    NET_BUFFER_LIST_NEXT_NBL(Send->Parent) = *Done;
    *Done = Send->Parent;

    ExFreePoolWithTag(Send, NWIFI_TAG);
}

static
VOID
NTAPI
NwifiSend(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PNWIFI_MODULE Module = FilterModuleContext;
    PNET_BUFFER_LIST Parent, Next, Frame;
    PNET_BUFFER_LIST Frames = NULL, Done = NULL;
    NDIS_STATUS Refused;
    PNET_BUFFER NetBuffer;
    PNWIFI_SEND Send;

    if (!Module->Running)
        Refused = NDIS_STATUS_PAUSED;
    else if (!Module->Associated)
        Refused = NDIS_STATUS_MEDIA_DISCONNECTED;
    else
        Refused = NDIS_STATUS_SUCCESS;

    for (Parent = NetBufferLists; Parent != NULL; Parent = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Parent);
        NET_BUFFER_LIST_NEXT_NBL(Parent) = NULL;

        Send = NULL;
        if (Refused == NDIS_STATUS_SUCCESS)
            Send = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Send), NWIFI_TAG);

        if (Send == NULL)
        {
            NET_BUFFER_LIST_STATUS(Parent) = (Refused != NDIS_STATUS_SUCCESS) ? Refused : NDIS_STATUS_RESOURCES;
            NET_BUFFER_LIST_NEXT_NBL(Parent) = Done;
            Done = Parent;
            continue;
        }

        /* One frame of ours per NET_BUFFER; the last one back finishes the parent */
        Send->Parent = Parent;
        Send->Pending = 1;
        Send->Status = NDIS_STATUS_SUCCESS;

        for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(Parent); NetBuffer != NULL; NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
        {
            Frame = NwifiBuildNative(Module, NetBuffer);
            if (Frame == NULL)
            {
                Send->Status = NDIS_STATUS_RESOURCES;
                continue;
            }

            NwifiCountFrame(Module);
            NWIFI_SEND_OF(Frame) = Send;
            InterlockedIncrement(&Send->Pending);
            NET_BUFFER_LIST_NEXT_NBL(Frame) = Frames;
            Frames = Frame;
        }

        if (InterlockedDecrement(&Send->Pending) == 0)
            NwifiFinishSend(Send, &Done);
    }

    if (Frames != NULL)
        NdisFSendNetBufferLists(Module->FilterHandle, Frames, PortNumber, SendFlags);

    if (Done != NULL)
        NdisFSendNetBufferListsComplete(Module->FilterHandle, Done, NwifiSendCompleteFlags());
}

static
VOID
NTAPI
NwifiSendComplete(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG SendCompleteFlags)
{
    PNWIFI_MODULE Module = FilterModuleContext;
    PNET_BUFFER_LIST Frame, Next, Done = NULL;
    PNWIFI_SEND Send;

    for (Frame = NetBufferLists; Frame != NULL; Frame = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Frame);
        Send = NWIFI_SEND_OF(Frame);

        if (NET_BUFFER_LIST_STATUS(Frame) != NDIS_STATUS_SUCCESS)
            Send->Status = NET_BUFFER_LIST_STATUS(Frame);

        NwifiFreeFrame(Module, Frame);

        if (InterlockedDecrement(&Send->Pending) == 0)
            NwifiFinishSend(Send, &Done);
    }

    if (Done != NULL)
        NdisFSendNetBufferListsComplete(Module->FilterHandle, Done, SendCompleteFlags);
}

/* Receive */

static
VOID
NTAPI
NwifiReceive(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG NumberOfNetBufferLists,
    _In_ ULONG ReceiveFlags)
{
    PNWIFI_MODULE Module = FilterModuleContext;
    PNET_BUFFER_LIST Received, Frame, Frames = NULL;
    PNET_BUFFER NetBuffer;
    ULONG Count = 0;

    UNREFERENCED_PARAMETER(NumberOfNetBufferLists);

    if (Module->Running)
    {
        for (Received = NetBufferLists; Received != NULL; Received = NET_BUFFER_LIST_NEXT_NBL(Received))
        {
            for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(Received); NetBuffer != NULL; NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
            {
                /* Only data frames go up, as Ethernet */
                Frame = NwifiBuildEthernet(Module, NetBuffer);
                if (Frame == NULL)
                    continue;

                NwifiCountFrame(Module);
                NET_BUFFER_LIST_NEXT_NBL(Frame) = Frames;
                Frames = Frame;
                Count++;
            }
        }
    }

    /* Everything was copied, so the miniport gets its lists back right away */
    if (!(ReceiveFlags & NDIS_RECEIVE_FLAGS_RESOURCES))
    {
        NdisFReturnNetBufferLists(Module->FilterHandle,
                                  NetBufferLists,
                                  (ReceiveFlags & NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL) ? NDIS_RETURN_FLAGS_DISPATCH_LEVEL : 0);
    }

    if (Frames != NULL)
    {
        NdisFIndicateReceiveNetBufferLists(Module->FilterHandle,
                                           Frames,
                                           PortNumber,
                                           Count,
                                           ReceiveFlags & ~NDIS_RECEIVE_FLAGS_RESOURCES);
    }
}

static
VOID
NTAPI
NwifiReturn(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    PNWIFI_MODULE Module = FilterModuleContext;
    PNET_BUFFER_LIST Frame, Next;

    UNREFERENCED_PARAMETER(ReturnFlags);

    for (Frame = NetBufferLists; Frame != NULL; Frame = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(Frame);
        NwifiFreeFrame(Module, Frame);
    }
}

/* Status */

static
VOID
NTAPI
NwifiStatus(
    _In_ NDIS_HANDLE FilterModuleContext,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    PNWIFI_MODULE Module = FilterModuleContext;

    NwifiTrackStatus(Module, StatusIndication);

    /* The WLAN service learns about scans and connections from these, and
       runs SAE for a WDI miniport that asks for it */
    if (NWIFI_IS_DOT11_STATUS(StatusIndication->StatusCode) ||
        StatusIndication->StatusCode == NDIS_STATUS_WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED)
    {
        NwifiQueueIndication(Module, StatusIndication);
    }

    NdisFIndicateStatus(Module->FilterHandle, StatusIndication);
}

/* Driver */

static
VOID
NTAPI
NwifiUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    NdisFDeregisterFilterDriver(NwifiDriverHandle);
    NwifiDeleteControlDevice();
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_FILTER_DRIVER_CHARACTERISTICS Chars;
    NDIS_STATUS Status;
    NTSTATUS NtStatus;

    UNREFERENCED_PARAMETER(RegistryPath);

    InitializeListHead(&NwifiModules);
    KeInitializeSpinLock(&NwifiModulesLock);

    NtStatus = NwifiCreateControlDevice(DriverObject);
    if (!NT_SUCCESS(NtStatus))
        return NtStatus;

    RtlZeroMemory(&Chars, sizeof(Chars));
    Chars.Header.Type = NDIS_OBJECT_TYPE_FILTER_DRIVER_CHARACTERISTICS;
    Chars.Header.Revision = NDIS_FILTER_CHARACTERISTICS_REVISION_1;
    Chars.Header.Size = NDIS_SIZEOF_FILTER_DRIVER_CHARACTERISTICS_REVISION_1;
    Chars.MajorNdisVersion = 6;
    Chars.MinorNdisVersion = 0;
    Chars.MajorDriverVersion = 1;
    RtlInitUnicodeString(&Chars.FriendlyName, L"Native WiFi Filter");
    RtlInitUnicodeString(&Chars.UniqueName, L"{E475CF9A-60CD-4439-A75F-0079CE0E18A1}");
    RtlInitUnicodeString(&Chars.ServiceName, L"NativeWifiP");
    Chars.AttachHandler = NwifiAttach;
    Chars.DetachHandler = NwifiDetach;
    Chars.RestartHandler = NwifiRestart;
    Chars.PauseHandler = NwifiPause;
    Chars.SendNetBufferListsHandler = NwifiSend;
    Chars.SendNetBufferListsCompleteHandler = NwifiSendComplete;
    Chars.ReceiveNetBufferListsHandler = NwifiReceive;
    Chars.ReturnNetBufferListsHandler = NwifiReturn;
    Chars.StatusHandler = NwifiStatus;
    Chars.OidRequestCompleteHandler = NwifiOidRequestComplete;

    DriverObject->DriverUnload = NwifiUnload;

    Status = NdisFRegisterFilterDriver(DriverObject, NULL, &Chars, &NwifiDriverHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("NdisFRegisterFilterDriver failed with 0x%lx\n", Status);
        NwifiDeleteControlDevice();
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}
