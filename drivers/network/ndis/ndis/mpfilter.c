/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Light-weight filter drivers and the filter stack of an adapter
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

/*
 * Filter modules sit between the miniport and the protocols, the first entry
 * of FilterStack nearest the protocols. Every path skips a module that has no
 * handler for it, and the stack only changes while the adapter is paused.
 */

#define CORE_FILTER_TAG 'tlFN'

typedef enum _CORE_FILTER_STATE
{
    CoreFilterAttaching,
    CoreFilterPaused,
    CoreFilterRestarting,
    CoreFilterRunning,
    CoreFilterPausing
} CORE_FILTER_STATE;

typedef struct _CORE_FILTER_DRIVER
{
    LIST_ENTRY ListEntry;
    PDRIVER_OBJECT DriverObject;
    NDIS_HANDLE Context;
    NDIS_FILTER_DRIVER_CHARACTERISTICS Chars;

    /* Bit n set means the filter binds to adapters of NDIS_MEDIUM n */
    ULONG MediaMask;

    /* Its modules, under CoreFilterLock */
    LIST_ENTRY Modules;
} CORE_FILTER_DRIVER, *PCORE_FILTER_DRIVER;

typedef struct _CORE_FILTER_MODULE
{
    LIST_ENTRY StackEntry;
    LIST_ENTRY DriverEntry;
    PCORE_FILTER_DRIVER Driver;
    PLOGICAL_ADAPTER Adapter;

    /* From NdisFSetAttributes */
    NDIS_HANDLE Context;
    BOOLEAN ContextSet;

    /* Found by the paths only once attached, under the adapter's core lock */
    BOOLEAN Attached;
    CORE_FILTER_STATE State;

    /* One reference for being attached, one for every call in flight */
    LONG References;
    KEVENT Unreferenced;

    /* NdisFPauseComplete or NdisFRestartComplete */
    KEVENT OperationDone;
    NDIS_STATUS OperationStatus;

    UNICODE_STRING GuidName;
} CORE_FILTER_MODULE, *PCORE_FILTER_MODULE;

/* Which handler a path looks for */
#define CORE_FILTER_HANDLER(_Field) FIELD_OFFSET(NDIS_FILTER_DRIVER_CHARACTERISTICS, _Field)

/* The module an OID request was sent down by, or NULL for NDIS itself */
#define CORE_FILTER_OID_ORIGIN(_Request) (*(PCORE_FILTER_MODULE *)&(_Request)->NdisReserved[0])

/* A filter's request on its way to the miniport, which needs a core request around it */
typedef struct _CORE_FILTER_OID
{
    CORE_OID_REQUEST Core;
    PNDIS_OID_REQUEST Original;
    PCORE_FILTER_MODULE Origin;
} CORE_FILTER_OID, *PCORE_FILTER_OID;

static LIST_ENTRY CoreFilterDrivers;
static FAST_MUTEX CoreFilterLock;

static const WCHAR CoreFilterClassKey[] =
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Network\\"
    L"{4d36e974-e325-11ce-bfc1-08002be10318}\\";

/**
 * @brief
 * Sets up the filter driver list when NDIS loads.
 */
VOID
NTAPI
CoreFilterInitialize(VOID)
{
    InitializeListHead(&CoreFilterDrivers);
    ExInitializeFastMutex(&CoreFilterLock);
}

/* Finding the next module */

static
PVOID
CoreFilterHandlerOf(
    _In_ PCORE_FILTER_MODULE Module,
    _In_ SIZE_T Handler)
{
    return *(PVOID *)((PUCHAR)&Module->Driver->Chars + Handler);
}

/*
 * The nearest attached module with the handler past From, or from the end of
 * the stack when From is NULL. Down walks toward the miniport.
 */
static
PCORE_FILTER_MODULE
CoreFilterFind(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PCORE_FILTER_MODULE From,
    _In_ BOOLEAN Down,
    _In_ SIZE_T Handler)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    PCORE_FILTER_MODULE Found = NULL;
    PCORE_FILTER_MODULE Module;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);

    if (From != NULL)
        Entry = Down ? From->StackEntry.Flink : From->StackEntry.Blink;
    else
        Entry = Down ? Core->FilterStack.Flink : Core->FilterStack.Blink;

    while (Entry != &Core->FilterStack)
    {
        Module = CONTAINING_RECORD(Entry, CORE_FILTER_MODULE, StackEntry);
        if (Module->Attached && CoreFilterHandlerOf(Module, Handler) != NULL)
        {
            InterlockedIncrement(&Module->References);
            Found = Module;
            break;
        }

        Entry = Down ? Entry->Flink : Entry->Blink;
    }

    KeReleaseSpinLock(&Core->Lock, OldIrql);
    return Found;
}

static
VOID
CoreFilterRelease(
    _In_ PCORE_FILTER_MODULE Module)
{
    if (InterlockedDecrement(&Module->References) == 0)
        KeSetEvent(&Module->Unreferenced, IO_NO_INCREMENT, FALSE);
}

/* Data */

/**
 * @brief
 * Passes sends down to the next filter below From, or to the miniport.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] From
 * The module sending, or NULL for the protocols.
 */
VOID
NTAPI
CoreFilterSend(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PCORE_FILTER_MODULE From,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PCORE_FILTER_MODULE Module;

    Module = CoreFilterFind(Adapter, From, TRUE, CORE_FILTER_HANDLER(SendNetBufferListsHandler));
    if (Module == NULL)
    {
        CoreSendToMiniport(Adapter, NetBufferLists, PortNumber, SendFlags);
        return;
    }

    Module->Driver->Chars.SendNetBufferListsHandler(Module->Context, NetBufferLists, PortNumber, SendFlags);
    CoreFilterRelease(Module);
}

/**
 * @brief
 * Passes send completions up to the next filter above From, or to the protocols.
 *
 * @param[in] From
 * The module completing, or NULL for the miniport.
 */
VOID
NTAPI
CoreFilterSendComplete(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PCORE_FILTER_MODULE From,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG SendCompleteFlags)
{
    PCORE_FILTER_MODULE Module;

    Module = CoreFilterFind(Adapter, From, FALSE, CORE_FILTER_HANDLER(SendNetBufferListsCompleteHandler));
    if (Module == NULL)
    {
        Pro5SendComplete(Adapter, NetBufferLists);
        return;
    }

    Module->Driver->Chars.SendNetBufferListsCompleteHandler(Module->Context, NetBufferLists, SendCompleteFlags);
    CoreFilterRelease(Module);
}

/**
 * @brief
 * Passes receives up to the next filter above From, or to the protocols.
 *
 * @param[in] From
 * The module indicating, or NULL for the miniport.
 */
VOID
NTAPI
CoreFilterIndicateReceive(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PCORE_FILTER_MODULE From,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG NumberOfNetBufferLists,
    _In_ ULONG ReceiveFlags)
{
    PCORE_FILTER_MODULE Module;

    Module = CoreFilterFind(Adapter, From, FALSE, CORE_FILTER_HANDLER(ReceiveNetBufferListsHandler));
    if (Module == NULL)
    {
        CoreIndicateToProtocols(Adapter, NetBufferLists, ReceiveFlags);
        return;
    }

    Module->Driver->Chars.ReceiveNetBufferListsHandler(Module->Context,
                                                       NetBufferLists,
                                                       PortNumber,
                                                       NumberOfNetBufferLists,
                                                       ReceiveFlags);
    CoreFilterRelease(Module);
}

/**
 * @brief
 * Returns received lists down to the next filter below From, or to the miniport.
 *
 * @param[in] From
 * The module returning, or NULL for the protocols.
 */
VOID
NTAPI
CoreFilterReturn(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PCORE_FILTER_MODULE From,
    _In_opt_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    PCORE_FILTER_MODULE Module;

    if (NetBufferLists == NULL)
        return;

    Module = CoreFilterFind(Adapter, From, TRUE, CORE_FILTER_HANDLER(ReturnNetBufferListsHandler));
    if (Module == NULL)
    {
        CoreReturnToMiniport(Adapter, NetBufferLists);
        return;
    }

    Module->Driver->Chars.ReturnNetBufferListsHandler(Module->Context, NetBufferLists, ReturnFlags);
    CoreFilterRelease(Module);
}

/* Status and PnP */

/**
 * @brief
 * Passes a status indication up to the next filter above From, or to the protocols.
 *
 * @param[in] From
 * The module indicating, or NULL for the miniport.
 */
VOID
NTAPI
CoreFilterIndicateStatus(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PCORE_FILTER_MODULE From,
    _In_ PNDIS_STATUS_INDICATION StatusIndication)
{
    PCORE_FILTER_MODULE Module;

    Module = CoreFilterFind(Adapter, From, FALSE, CORE_FILTER_HANDLER(StatusHandler));
    if (Module == NULL)
    {
        CoreIndicateStatusToProtocols(Adapter, StatusIndication);
        return;
    }

    Module->Driver->Chars.StatusHandler(Module->Context, StatusIndication);
    CoreFilterRelease(Module);
}

/**
 * @brief
 * Passes a network PnP event up to the next filter above From, or to the protocols.
 *
 * @param[in] From
 * The module passing it on, or NULL for the miniport.
 *
 * @return
 * The outcome the layers above report.
 */
NDIS_STATUS
NTAPI
CoreFilterNetPnPEvent(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PCORE_FILTER_MODULE From,
    _In_ PNET_PNP_EVENT_NOTIFICATION NetPnPEventNotification)
{
    PCORE_FILTER_MODULE Module;
    NDIS_STATUS Status;

    Module = CoreFilterFind(Adapter, From, FALSE, CORE_FILTER_HANDLER(NetPnPEventHandler));
    if (Module == NULL)
        return CoreNotifyProtocols(Adapter, &NetPnPEventNotification->NetPnPEvent);

    Status = Module->Driver->Chars.NetPnPEventHandler(Module->Context, NetPnPEventNotification);
    CoreFilterRelease(Module);
    return Status;
}

/* OID requests */

static
VOID
NTAPI
CoreFilterMiniportOidDone(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest,
    _In_ NDIS_STATUS Status)
{
    PCORE_FILTER_OID FilterOid = CONTAINING_RECORD(CoreRequest, CORE_FILTER_OID, Core);
    PNDIS_OID_REQUEST Original = FilterOid->Original;
    PCORE_FILTER_MODULE Origin = FilterOid->Origin;

    UNREFERENCED_PARAMETER(Adapter);

    /* The results are the only part the miniport wrote */
    Original->DATA = CoreRequest->Request.DATA;
    ExFreePoolWithTag(FilterOid, CORE_FILTER_TAG);

    Origin->Driver->Chars.OidRequestCompleteHandler(Origin->Context, Original, Status);
}

static
NDIS_STATUS
CoreFilterOidToMiniport(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_FILTER_MODULE Origin,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PCORE_FILTER_OID FilterOid;
    NDIS_STATUS Status;

    FilterOid = ExAllocatePoolWithTag(NonPagedPool, sizeof(*FilterOid), CORE_FILTER_TAG);
    if (FilterOid == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(FilterOid, sizeof(*FilterOid));
    FilterOid->Core.Request = *OidRequest;
    FilterOid->Core.Completion = CoreFilterMiniportOidDone;
    FilterOid->Original = OidRequest;
    FilterOid->Origin = Origin;

    Status = CoreOidRequest(Adapter, &FilterOid->Core);
    if (Status == NDIS_STATUS_PENDING)
        return Status;

    OidRequest->DATA = FilterOid->Core.Request.DATA;
    ExFreePoolWithTag(FilterOid, CORE_FILTER_TAG);
    return Status;
}

/**
 * @brief
 * The entry point for OID requests from the protocols, the top of the stack.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] CoreRequest
 * The request. Completion is called only if this returns NDIS_STATUS_PENDING.
 *
 * @return
 * The request's status, or NDIS_STATUS_PENDING.
 */
NDIS_STATUS
NTAPI
CoreStackOidRequest(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest)
{
    PCORE_FILTER_MODULE Module;
    NDIS_STATUS Status;

    Module = CoreFilterFind(Adapter, NULL, TRUE, CORE_FILTER_HANDLER(OidRequestHandler));
    if (Module == NULL)
        return CoreOidRequest(Adapter, CoreRequest);

    CORE_FILTER_OID_ORIGIN(&CoreRequest->Request) = NULL;
    Status = Module->Driver->Chars.OidRequestHandler(Module->Context, &CoreRequest->Request);
    CoreFilterRelease(Module);
    return Status;
}

/* Registration and binding */

static
ULONG
CoreFilterMediumBit(
    _In_ PCWSTR Name,
    _In_ SIZE_T Length)
{
    static const struct
    {
        PCWSTR Name;
        NDIS_MEDIUM Medium;
    } Media[] =
    {
        { L"ethernet", NdisMedium802_3 },
        { L"wlan", NdisMediumNative802_11 },
        { L"tokenring", NdisMedium802_5 },
        { L"fddi", NdisMediumFddi },
        { L"wan", NdisMediumWan },
        { L"ppip", NdisMediumIP },
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(Media); i++)
    {
        if (wcslen(Media[i].Name) == Length && _wcsnicmp(Media[i].Name, Name, Length) == 0)
            return 1UL << Media[i].Medium;
    }

    return 0;
}

/*
 * The media a filter binds to come from the FilterMediaTypes value its INF
 * writes under Ndi\Interfaces in the network service class key.
 */
static
ULONG
CoreFilterReadMedia(
    _In_ PCUNICODE_STRING UniqueName)
{
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 256 * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"FilterMediaTypes");
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING KeyName;
    HANDLE Key;
    PWCHAR Text, End, Token;
    ULONG Length, Mask = 0;
    NTSTATUS Status;

    KeyName.Length = 0;
    KeyName.MaximumLength = sizeof(CoreFilterClassKey) + UniqueName->Length + 64;
    KeyName.Buffer = ExAllocatePoolWithTag(PagedPool, KeyName.MaximumLength, CORE_FILTER_TAG);
    if (KeyName.Buffer == NULL)
        return 0;

    RtlAppendUnicodeToString(&KeyName, CoreFilterClassKey);
    RtlAppendUnicodeStringToString(&KeyName, UniqueName);
    RtlAppendUnicodeToString(&KeyName, L"\\Ndi\\Interfaces");

    InitializeObjectAttributes(&Attributes, &KeyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_QUERY_VALUE, &Attributes);
    ExFreePoolWithTag(KeyName.Buffer, CORE_FILTER_TAG);
    if (!NT_SUCCESS(Status))
        return 0;

    RtlZeroMemory(Buffer, sizeof(Buffer));
    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, sizeof(Buffer) - sizeof(WCHAR), &Length);
    ZwClose(Key);
    if (!NT_SUCCESS(Status) || Value->Type != REG_SZ)
        return 0;

    /* A comma separated list such as "ethernet, wlan" */
    Text = (PWCHAR)Value->Data;
    End = Text + Value->DataLength / sizeof(WCHAR);
    while (Text < End && *Text != UNICODE_NULL)
    {
        while (Text < End && (*Text == L',' || *Text == L' '))
            Text++;

        Token = Text;
        while (Text < End && *Text != UNICODE_NULL && *Text != L',' && *Text != L' ')
            Text++;

        if (Text > Token)
            Mask |= CoreFilterMediumBit(Token, Text - Token);
    }

    return Mask;
}

static
BOOLEAN
CoreFilterBindsTo(
    _In_ PCORE_FILTER_DRIVER Driver,
    _In_ PLOGICAL_ADAPTER Adapter)
{
    NDIS_MEDIUM Medium = Adapter->Core.MediaType;

    return (ULONG)Medium < 32 && (Driver->MediaMask & (1UL << Medium)) != 0;
}

/* The module name is {adapter}-{filter}-0000, as a filter expects */
static
NTSTATUS
CoreFilterBuildGuidName(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_FILTER_DRIVER Driver,
    _Out_ PUNICODE_STRING GuidName)
{
    PUNICODE_STRING MiniportName = &Adapter->NdisMiniportBlock.MiniportName;
    UNICODE_STRING AdapterGuid;
    USHORT Skip = 0;

    /* The miniport name is \DEVICE\{GUID} */
    while (Skip < MiniportName->Length / sizeof(WCHAR) && MiniportName->Buffer[Skip] != L'{')
        Skip++;

    AdapterGuid.Buffer = MiniportName->Buffer + Skip;
    AdapterGuid.Length = MiniportName->Length - Skip * sizeof(WCHAR);
    AdapterGuid.MaximumLength = AdapterGuid.Length;

    GuidName->Length = 0;
    GuidName->MaximumLength = AdapterGuid.Length + Driver->Chars.UniqueName.Length + 7 * sizeof(WCHAR);
    GuidName->Buffer = ExAllocatePoolWithTag(NonPagedPool, GuidName->MaximumLength, CORE_FILTER_TAG);
    if (GuidName->Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlAppendUnicodeStringToString(GuidName, &AdapterGuid);
    RtlAppendUnicodeToString(GuidName, L"-");
    RtlAppendUnicodeStringToString(GuidName, &Driver->Chars.UniqueName);
    RtlAppendUnicodeToString(GuidName, L"-0000");
    return STATUS_SUCCESS;
}

static
VOID
CoreFilterFreeModule(
    _In_ PCORE_FILTER_MODULE Module)
{
    if (Module->GuidName.Buffer != NULL)
        ExFreePoolWithTag(Module->GuidName.Buffer, CORE_FILTER_TAG);
    ExFreePoolWithTag(Module, CORE_FILTER_TAG);
}

/* Called with CoreFilterLock held and the adapter paused */
static
VOID
CoreFilterAttach(
    _In_ PCORE_FILTER_DRIVER Driver,
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_FILTER_ATTACH_PARAMETERS Parameters;
    NDIS_OFFLOAD Offload;
    PCORE_FILTER_MODULE Module;
    NDIS_STATUS Status;
    KIRQL OldIrql;

    Module = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Module), CORE_FILTER_TAG);
    if (Module == NULL)
        return;

    RtlZeroMemory(Module, sizeof(*Module));
    Module->Driver = Driver;
    Module->Adapter = Adapter;
    Module->State = CoreFilterAttaching;
    Module->References = 1;
    KeInitializeEvent(&Module->Unreferenced, NotificationEvent, FALSE);
    KeInitializeEvent(&Module->OperationDone, NotificationEvent, FALSE);

    if (!NT_SUCCESS(CoreFilterBuildGuidName(Adapter, Driver, &Module->GuidName)))
    {
        CoreFilterFreeModule(Module);
        return;
    }

    RtlZeroMemory(&Offload, sizeof(Offload));
    Offload.Header.Type = NDIS_OBJECT_TYPE_OFFLOAD;
    Offload.Header.Revision = NDIS_OFFLOAD_REVISION_6;
    Offload.Header.Size = NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_6;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_FILTER_ATTACH_PARAMETERS;
    Parameters.Header.Revision = NDIS_FILTER_ATTACH_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_FILTER_ATTACH_PARAMETERS_REVISION_1;
    Parameters.IfIndex = Adapter->Interface.IfIndex;
    Parameters.NetLuid = Adapter->Interface.NetLuid;
    Parameters.FilterModuleGuidName = &Module->GuidName;
    Parameters.BaseMiniportIfIndex = Adapter->Interface.IfIndex;
    Parameters.BaseMiniportInstanceName = &Adapter->NdisMiniportBlock.MiniportName;
    Parameters.BaseMiniportName = &Adapter->NdisMiniportBlock.MiniportName;
    Parameters.MediaConnectState = Core->LinkState.MediaConnectState;
    Parameters.MediaDuplexState = Core->LinkState.MediaDuplexState;
    Parameters.XmitLinkSpeed = Core->LinkState.XmitLinkSpeed;
    Parameters.RcvLinkSpeed = Core->LinkState.RcvLinkSpeed;
    Parameters.MiniportMediaType = Core->MediaType;
    Parameters.MiniportPhysicalMediaType = Core->PhysicalMediumType;
    Parameters.DefaultOffloadConfiguration = &Offload;
    Parameters.MacAddressLength = Core->MacAddressLength;
    RtlCopyMemory(Parameters.CurrentMacAddress, Core->CurrentMacAddress, sizeof(Parameters.CurrentMacAddress));
    Parameters.BaseMiniportNetLuid = Adapter->Interface.NetLuid;
    Parameters.LowerIfIndex = Adapter->Interface.IfIndex;
    Parameters.LowerIfNetLuid = Adapter->Interface.NetLuid;

    /* The newest module sits on top. It is in the stack, unattached, so its
       own OID requests during attach find what is below it. */
    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    InsertHeadList(&Core->FilterStack, &Module->StackEntry);
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    Status = Driver->Chars.AttachHandler((NDIS_HANDLE)Module, Driver->Context, &Parameters);
    if (Status == NDIS_STATUS_SUCCESS && !Module->ContextSet)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Filter %wZ attached without NdisFSetAttributes.\n", &Driver->Chars.FriendlyName));
        Driver->Chars.DetachHandler(Module->Context);
        Status = NDIS_STATUS_FAILURE;
    }

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    if (Status == NDIS_STATUS_SUCCESS)
    {
        Module->Attached = TRUE;
        Module->State = CoreFilterPaused;
    }
    else
    {
        RemoveEntryList(&Module->StackEntry);
    }
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Filter %wZ did not attach to %wZ (0x%x).\n",
                                  &Driver->Chars.FriendlyName,
                                  &Adapter->NdisMiniportBlock.MiniportName,
                                  Status));
        CoreFilterFreeModule(Module);
        return;
    }

    InsertTailList(&Driver->Modules, &Module->DriverEntry);
}

/* Called with CoreFilterLock held and the adapter paused */
static
VOID
CoreFilterDetach(
    _In_ PCORE_FILTER_MODULE Module)
{
    PMINIPORT_CORE Core = &Module->Adapter->Core;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Module->Attached = FALSE;
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    /* Nothing reaches it any more once the calls already in it return */
    if (InterlockedDecrement(&Module->References) != 0)
        KeWaitForSingleObject(&Module->Unreferenced, Executive, KernelMode, FALSE, NULL);

    Module->Driver->Chars.DetachHandler(Module->Context);

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    RemoveEntryList(&Module->StackEntry);
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    RemoveEntryList(&Module->DriverEntry);
    CoreFilterFreeModule(Module);
}

/**
 * @brief
 * Attaches every filter that binds to a new adapter, before its first restart.
 *
 * @param[in] Adapter
 * The adapter, paused.
 */
VOID
NTAPI
CoreFilterAttachAll(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PCORE_FILTER_DRIVER Driver;
    PLIST_ENTRY Entry;

    ExAcquireFastMutex(&CoreFilterLock);

    for (Entry = CoreFilterDrivers.Flink; Entry != &CoreFilterDrivers; Entry = Entry->Flink)
    {
        Driver = CONTAINING_RECORD(Entry, CORE_FILTER_DRIVER, ListEntry);
        if (CoreFilterBindsTo(Driver, Adapter))
            CoreFilterAttach(Driver, Adapter);
    }

    ExReleaseFastMutex(&CoreFilterLock);
}

/**
 * @brief
 * Detaches every filter from an adapter going away.
 *
 * @param[in] Adapter
 * The adapter, paused.
 */
VOID
NTAPI
CoreFilterDetachAll(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;

    ExAcquireFastMutex(&CoreFilterLock);

    while (!IsListEmpty(&Core->FilterStack))
        CoreFilterDetach(CONTAINING_RECORD(Core->FilterStack.Flink, CORE_FILTER_MODULE, StackEntry));

    ExReleaseFastMutex(&CoreFilterLock);
}

/* Pause and restart */

static
NDIS_STATUS
CoreFilterWait(
    _In_ PCORE_FILTER_MODULE Module,
    _In_ NDIS_STATUS Status)
{
    if (Status == NDIS_STATUS_PENDING)
    {
        KeWaitForSingleObject(&Module->OperationDone, Executive, KernelMode, FALSE, NULL);
        Status = Module->OperationStatus;
    }

    KeClearEvent(&Module->OperationDone);
    return Status;
}

/**
 * @brief
 * Pauses the running filters of an adapter, nearest the protocols first.
 *
 * @param[in] Adapter
 * The adapter, whose data path is already closed to the protocols.
 */
VOID
NTAPI
CoreFilterPauseStack(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_FILTER_PAUSE_PARAMETERS Parameters;
    PCORE_FILTER_MODULE Module;
    PLIST_ENTRY Entry;
    NDIS_STATUS Status;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_FILTER_PAUSE_PARAMETERS;
    Parameters.Header.Revision = NDIS_FILTER_PAUSE_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_FILTER_PAUSE_PARAMETERS_REVISION_1;
    Parameters.PauseReason = NDIS_PAUSE_NDIS_INTERNAL;

    for (Entry = Core->FilterStack.Flink; Entry != &Core->FilterStack; Entry = Entry->Flink)
    {
        Module = CONTAINING_RECORD(Entry, CORE_FILTER_MODULE, StackEntry);
        if (Module->State != CoreFilterRunning)
            continue;

        Module->State = CoreFilterPausing;
        Status = Module->Driver->Chars.PauseHandler(Module->Context, &Parameters);
        CoreFilterWait(Module, Status);
        Module->State = CoreFilterPaused;
    }
}

/**
 * @brief
 * Restarts the paused filters of an adapter, nearest the miniport first.
 *
 * @param[in] Adapter
 * The adapter, whose miniport already restarted.
 *
 * @param[in] RestartAttributes
 * What the miniport was restarted with.
 *
 * @return
 * NDIS_STATUS_SUCCESS when every filter runs.
 */
NDIS_STATUS
NTAPI
CoreFilterRestartStack(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PNDIS_RESTART_ATTRIBUTES RestartAttributes)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_FILTER_RESTART_PARAMETERS Parameters;
    PCORE_FILTER_MODULE Module;
    PLIST_ENTRY Entry;
    NDIS_STATUS Status;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_FILTER_RESTART_PARAMETERS;
    Parameters.Header.Revision = NDIS_FILTER_RESTART_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF__FILTER_RESTART_PARAMETERS_REVISION_1;
    Parameters.MiniportMediaType = Core->MediaType;
    Parameters.MiniportPhysicalMediaType = Core->PhysicalMediumType;
    Parameters.RestartAttributes = RestartAttributes;
    Parameters.LowerIfIndex = Adapter->Interface.IfIndex;
    Parameters.LowerIfNetLuid = Adapter->Interface.NetLuid;

    for (Entry = Core->FilterStack.Blink; Entry != &Core->FilterStack; Entry = Entry->Blink)
    {
        Module = CONTAINING_RECORD(Entry, CORE_FILTER_MODULE, StackEntry);
        if (Module->State != CoreFilterPaused)
            continue;

        Module->State = CoreFilterRestarting;
        Status = Module->Driver->Chars.RestartHandler(Module->Context, &Parameters);
        Status = CoreFilterWait(Module, Status);

        if (Status != NDIS_STATUS_SUCCESS)
        {
            /* The layers above stay paused along with it */
            NDIS_DbgPrint(MIN_TRACE, ("Filter %wZ failed to restart (0x%x).\n",
                                      &Module->Driver->Chars.FriendlyName, Status));
            Module->State = CoreFilterPaused;
            return Status;
        }

        Module->State = CoreFilterRunning;
    }

    return NDIS_STATUS_SUCCESS;
}

/* Exports */

/**
 * @brief
 * Registers a light-weight filter driver and attaches it to every running
 * adapter it binds to.
 *
 * @param[in] DriverObject
 * The filter driver.
 *
 * @param[in] FilterDriverContext
 * Handed back to FilterAttach.
 *
 * @param[in] FilterDriverCharacteristics
 * The filter's handlers and names.
 *
 * @param[out] NdisFilterDriverHandle
 * Receives the handle for NdisFDeregisterFilterDriver.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or why the filter was refused.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisFRegisterFilterDriver(
    PDRIVER_OBJECT DriverObject,
    NDIS_HANDLE FilterDriverContext,
    PNDIS_FILTER_DRIVER_CHARACTERISTICS FilterDriverCharacteristics,
    PNDIS_HANDLE NdisFilterDriverHandle)
{
    PNDIS_FILTER_DRIVER_CHARACTERISTICS Chars = FilterDriverCharacteristics;
    PLOGICAL_ADAPTER Adapters[32];
    PCORE_FILTER_DRIVER Driver;
    PLOGICAL_ADAPTER Adapter;
    ULONG Count = 0, Size, i;
    PLIST_ENTRY Entry;
    PWCHAR Strings;
    KIRQL OldIrql;

    *NdisFilterDriverHandle = NULL;

    if (Chars->Header.Type != NDIS_OBJECT_TYPE_FILTER_DRIVER_CHARACTERISTICS ||
        Chars->Header.Size < NDIS_SIZEOF_FILTER_DRIVER_CHARACTERISTICS_REVISION_1 ||
        Chars->MajorNdisVersion < 6)
    {
        return NDIS_STATUS_BAD_VERSION;
    }

    /* Every filter has to take part in attach, detach, pause and restart */
    if (Chars->AttachHandler == NULL || Chars->DetachHandler == NULL ||
        Chars->RestartHandler == NULL || Chars->PauseHandler == NULL)
    {
        return NDIS_STATUS_BAD_CHARACTERISTICS;
    }

    Size = sizeof(*Driver) + Chars->FriendlyName.Length + Chars->UniqueName.Length + Chars->ServiceName.Length;
    Driver = ExAllocatePoolWithTag(NonPagedPool, Size, CORE_FILTER_TAG);
    if (Driver == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Driver, Size);
    RtlCopyMemory(&Driver->Chars, Chars, min(Chars->Header.Size, sizeof(Driver->Chars)));
    Driver->DriverObject = DriverObject;
    Driver->Context = FilterDriverContext;
    InitializeListHead(&Driver->Modules);

    /* The names live on with the driver, not with the caller's buffers */
    Strings = (PWCHAR)(Driver + 1);
    Driver->Chars.FriendlyName.Buffer = Strings;
    RtlCopyMemory(Strings, Chars->FriendlyName.Buffer, Chars->FriendlyName.Length);
    Driver->Chars.FriendlyName.MaximumLength = Chars->FriendlyName.Length;
    Strings += Chars->FriendlyName.Length / sizeof(WCHAR);
    Driver->Chars.UniqueName.Buffer = Strings;
    RtlCopyMemory(Strings, Chars->UniqueName.Buffer, Chars->UniqueName.Length);
    Driver->Chars.UniqueName.MaximumLength = Chars->UniqueName.Length;
    Strings += Chars->UniqueName.Length / sizeof(WCHAR);
    Driver->Chars.ServiceName.Buffer = Strings;
    RtlCopyMemory(Strings, Chars->ServiceName.Buffer, Chars->ServiceName.Length);
    Driver->Chars.ServiceName.MaximumLength = Chars->ServiceName.Length;

    Driver->MediaMask = CoreFilterReadMedia(&Driver->Chars.UniqueName);
    if (Driver->MediaMask == 0)
        NDIS_DbgPrint(MIN_TRACE, ("Filter %wZ binds to no media.\n", &Driver->Chars.FriendlyName));

    if (Driver->Chars.SetOptionsHandler != NULL &&
        Driver->Chars.SetOptionsHandler((NDIS_HANDLE)Driver, FilterDriverContext) != NDIS_STATUS_SUCCESS)
    {
        ExFreePoolWithTag(Driver, CORE_FILTER_TAG);
        return NDIS_STATUS_FAILURE;
    }

    ExAcquireFastMutex(&CoreFilterLock);
    InsertTailList(&CoreFilterDrivers, &Driver->ListEntry);

    /* Adapters that are up get the new filter now. Ones still starting get it
       from CoreFilterAttachAll, and halting ones need CoreFilterLock first. */
    KeAcquireSpinLock(&AdapterListLock, &OldIrql);
    for (Entry = AdapterListHead.Flink; Entry != &AdapterListHead && Count < RTL_NUMBER_OF(Adapters); Entry = Entry->Flink)
    {
        Adapter = CONTAINING_RECORD(Entry, LOGICAL_ADAPTER, ListEntry);
        if ((Adapter->Core.State == CoreMiniportRunning || Adapter->Core.State == CoreMiniportPaused) &&
            CoreFilterBindsTo(Driver, Adapter))
        {
            Adapters[Count++] = Adapter;
        }
    }
    KeReleaseSpinLock(&AdapterListLock, OldIrql);

    for (i = 0; i < Count; i++)
    {
        CoreHoldPaused(Adapters[i], CORE_PAUSE_FILTER);
        CoreFilterAttach(Driver, Adapters[i]);
        CoreReleasePaused(Adapters[i], CORE_PAUSE_FILTER);
    }

    ExReleaseFastMutex(&CoreFilterLock);

    *NdisFilterDriverHandle = (NDIS_HANDLE)Driver;
    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Detaches a filter driver from every adapter and forgets it.
 *
 * @param[in] NdisFilterDriverHandle
 * From NdisFRegisterFilterDriver.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisFDeregisterFilterDriver(
    NDIS_HANDLE NdisFilterDriverHandle)
{
    PCORE_FILTER_DRIVER Driver = (PCORE_FILTER_DRIVER)NdisFilterDriverHandle;
    PCORE_FILTER_MODULE Module;
    PLOGICAL_ADAPTER Adapter;

    ExAcquireFastMutex(&CoreFilterLock);

    while (!IsListEmpty(&Driver->Modules))
    {
        Module = CONTAINING_RECORD(Driver->Modules.Flink, CORE_FILTER_MODULE, DriverEntry);
        Adapter = Module->Adapter;

        CoreHoldPaused(Adapter, CORE_PAUSE_FILTER);
        CoreFilterDetach(Module);
        CoreReleasePaused(Adapter, CORE_PAUSE_FILTER);
    }

    RemoveEntryList(&Driver->ListEntry);
    ExReleaseFastMutex(&CoreFilterLock);

    ExFreePoolWithTag(Driver, CORE_FILTER_TAG);
}

/**
 * @brief
 * Gives NDIS the context of a module being attached.
 *
 * @param[in] NdisFilterHandle
 * The module.
 *
 * @param[in] FilterModuleContext
 * What NDIS hands the filter's handlers.
 *
 * @param[in] FilterAttributes
 * The module's attributes.
 *
 * @return
 * NDIS_STATUS_SUCCESS or NDIS_STATUS_INVALID_PARAMETER.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisFSetAttributes(
    NDIS_HANDLE NdisFilterHandle,
    NDIS_HANDLE FilterModuleContext,
    PNDIS_FILTER_ATTRIBUTES FilterAttributes)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    if (FilterAttributes->Header.Type != NDIS_OBJECT_TYPE_FILTER_ATTRIBUTES ||
        FilterAttributes->Header.Size < NDIS_SIZEOF_FILTER_ATTRIBUTES_REVISION_1 ||
        Module->State != CoreFilterAttaching)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    Module->Context = FilterModuleContext;
    Module->ContextSet = TRUE;
    return NDIS_STATUS_SUCCESS;
}

typedef struct _CORE_FILTER_RESTART_WORK
{
    WORK_QUEUE_ITEM WorkItem;
    PLOGICAL_ADAPTER Adapter;
} CORE_FILTER_RESTART_WORK, *PCORE_FILTER_RESTART_WORK;

static
VOID
NTAPI
CoreFilterRestartWorker(
    _In_ PVOID Context)
{
    PCORE_FILTER_RESTART_WORK Work = Context;

    CoreHoldPaused(Work->Adapter, CORE_PAUSE_FILTER);
    CoreReleasePaused(Work->Adapter, CORE_PAUSE_FILTER);

    ExFreePoolWithTag(Work, CORE_FILTER_TAG);
}

/**
 * @brief
 * A filter asks for its stack to be paused and restarted, to renegotiate what
 * it offers the layers above.
 *
 * @param[in] NdisFilterHandle
 * The module.
 *
 * @return
 * NDIS_STATUS_SUCCESS once the restart is under way.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisFRestartFilter(
    NDIS_HANDLE NdisFilterHandle)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PCORE_FILTER_RESTART_WORK Work;

    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), CORE_FILTER_TAG);
    if (Work == NULL)
        return NDIS_STATUS_RESOURCES;

    Work->Adapter = Module->Adapter;
    ExInitializeWorkItem(&Work->WorkItem, CoreFilterRestartWorker, Work);
    ExQueueWorkItem(&Work->WorkItem, DelayedWorkQueue);
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFSendNetBufferLists(
    NDIS_HANDLE NdisFilterHandle,
    PNET_BUFFER_LIST NetBufferList,
    NDIS_PORT_NUMBER PortNumber,
    ULONG SendFlags)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    CoreFilterSend(Module->Adapter, Module, NetBufferList, PortNumber, SendFlags);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFSendNetBufferListsComplete(
    NDIS_HANDLE NdisFilterHandle,
    PNET_BUFFER_LIST NetBufferList,
    ULONG SendCompleteFlags)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    CoreFilterSendComplete(Module->Adapter, Module, NetBufferList, SendCompleteFlags);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFIndicateReceiveNetBufferLists(
    NDIS_HANDLE NdisFilterHandle,
    PNET_BUFFER_LIST NetBufferLists,
    NDIS_PORT_NUMBER PortNumber,
    ULONG NumberOfNetBufferLists,
    ULONG ReceiveFlags)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    CoreFilterIndicateReceive(Module->Adapter, Module, NetBufferLists, PortNumber, NumberOfNetBufferLists, ReceiveFlags);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFReturnNetBufferLists(
    NDIS_HANDLE NdisFilterHandle,
    PNET_BUFFER_LIST NetBufferLists,
    ULONG ReturnFlags)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    CoreFilterReturn(Module->Adapter, Module, NetBufferLists, ReturnFlags);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFCancelSendNetBufferLists(
    NDIS_HANDLE NdisFilterHandle,
    PVOID CancelId)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PLOGICAL_ADAPTER Adapter = Module->Adapter;
    PCORE_FILTER_MODULE Lower;

    Lower = CoreFilterFind(Adapter, Module, TRUE, CORE_FILTER_HANDLER(CancelSendNetBufferListsHandler));
    if (Lower != NULL)
    {
        Lower->Driver->Chars.CancelSendNetBufferListsHandler(Lower->Context, CancelId);
        CoreFilterRelease(Lower);
    }
    else if (Adapter->Core.Dispatch->CancelSendHandler != NULL)
    {
        Adapter->Core.Dispatch->CancelSendHandler(CORE_DISPATCH_CONTEXT(Adapter), CancelId);
    }
}

/**
 * @brief
 * A filter sends an OID request down, its own or one from above.
 *
 * @param[in] NdisFilterHandle
 * The module sending it.
 *
 * @param[in] OidRequest
 * The request. The module's FilterOidRequestComplete gets it back when this
 * returns NDIS_STATUS_PENDING.
 *
 * @return
 * The request's status, or NDIS_STATUS_PENDING.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisFOidRequest(
    NDIS_HANDLE NdisFilterHandle,
    PNDIS_OID_REQUEST OidRequest)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PLOGICAL_ADAPTER Adapter = Module->Adapter;
    PCORE_FILTER_MODULE Lower;
    NDIS_STATUS Status;

    Lower = CoreFilterFind(Adapter, Module, TRUE, CORE_FILTER_HANDLER(OidRequestHandler));
    if (Lower == NULL)
        return CoreFilterOidToMiniport(Adapter, Module, OidRequest);

    CORE_FILTER_OID_ORIGIN(OidRequest) = Module;
    Status = Lower->Driver->Chars.OidRequestHandler(Lower->Context, OidRequest);
    CoreFilterRelease(Lower);
    return Status;
}

/**
 * @brief
 * A filter finishes an OID request it pended.
 *
 * @param[in] NdisFilterHandle
 * The module finishing it.
 *
 * @param[in] OidRequest
 * The request as the module was given it.
 *
 * @param[in] Status
 * How it ended.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisFOidRequestComplete(
    NDIS_HANDLE NdisFilterHandle,
    PNDIS_OID_REQUEST OidRequest,
    NDIS_STATUS Status)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PCORE_FILTER_MODULE Origin = CORE_FILTER_OID_ORIGIN(OidRequest);
    PCORE_OID_REQUEST CoreRequest;

    if (Origin != NULL)
    {
        Origin->Driver->Chars.OidRequestCompleteHandler(Origin->Context, OidRequest, Status);
        return;
    }

    /* It came from the protocols through CoreStackOidRequest */
    CoreRequest = CONTAINING_RECORD(OidRequest, CORE_OID_REQUEST, Request);
    CoreRequest->Completion(Module->Adapter, CoreRequest, Status);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFCancelOidRequest(
    NDIS_HANDLE NdisFilterHandle,
    PVOID RequestId)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PLOGICAL_ADAPTER Adapter = Module->Adapter;
    PCORE_FILTER_MODULE Lower;

    Lower = CoreFilterFind(Adapter, Module, TRUE, CORE_FILTER_HANDLER(CancelOidRequestHandler));
    if (Lower != NULL)
    {
        Lower->Driver->Chars.CancelOidRequestHandler(Lower->Context, RequestId);
        CoreFilterRelease(Lower);
    }
    else if (Adapter->Core.Dispatch->CancelOidRequestHandler != NULL)
    {
        Adapter->Core.Dispatch->CancelOidRequestHandler(CORE_DISPATCH_CONTEXT(Adapter), RequestId);
    }
}

/**
 * @brief
 * A filter sends a direct OID request down. Only filters take them here; the
 * miniport direct path is not supported.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisFDirectOidRequest(
    NDIS_HANDLE NdisFilterHandle,
    PNDIS_OID_REQUEST OidRequest)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PCORE_FILTER_MODULE Lower;
    NDIS_STATUS Status;

    Lower = CoreFilterFind(Module->Adapter, Module, TRUE, CORE_FILTER_HANDLER(DirectOidRequestHandler));
    if (Lower == NULL)
        return NDIS_STATUS_NOT_SUPPORTED;

    CORE_FILTER_OID_ORIGIN(OidRequest) = Module;
    Status = Lower->Driver->Chars.DirectOidRequestHandler(Lower->Context, OidRequest);
    CoreFilterRelease(Lower);
    return Status;
}

_Use_decl_annotations_
VOID
NTAPI
NdisFDirectOidRequestComplete(
    NDIS_HANDLE NdisFilterHandle,
    PNDIS_OID_REQUEST OidRequest,
    NDIS_STATUS Status)
{
    PCORE_FILTER_MODULE Origin = CORE_FILTER_OID_ORIGIN(OidRequest);

    UNREFERENCED_PARAMETER(NdisFilterHandle);

    if (Origin != NULL && Origin->Driver->Chars.DirectOidRequestCompleteHandler != NULL)
        Origin->Driver->Chars.DirectOidRequestCompleteHandler(Origin->Context, OidRequest, Status);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFCancelDirectOidRequest(
    NDIS_HANDLE NdisFilterHandle,
    PVOID RequestId)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PCORE_FILTER_MODULE Lower;

    Lower = CoreFilterFind(Module->Adapter, Module, TRUE, CORE_FILTER_HANDLER(CancelDirectOidRequestHandler));
    if (Lower != NULL)
    {
        Lower->Driver->Chars.CancelDirectOidRequestHandler(Lower->Context, RequestId);
        CoreFilterRelease(Lower);
    }
}

_Use_decl_annotations_
VOID
NTAPI
NdisFIndicateStatus(
    NDIS_HANDLE NdisFilterHandle,
    PNDIS_STATUS_INDICATION StatusIndication)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    CoreFilterIndicateStatus(Module->Adapter, Module, StatusIndication);
}

_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisFNetPnPEvent(
    NDIS_HANDLE NdisFilterHandle,
    PNET_PNP_EVENT_NOTIFICATION NetPnPEventNotification)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    return CoreFilterNetPnPEvent(Module->Adapter, Module, NetPnPEventNotification);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFDevicePnPEventNotify(
    NDIS_HANDLE NdisFilterHandle,
    PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;
    PLOGICAL_ADAPTER Adapter = Module->Adapter;
    PCORE_FILTER_MODULE Lower;

    Lower = CoreFilterFind(Adapter, Module, TRUE, CORE_FILTER_HANDLER(DevicePnPEventNotifyHandler));
    if (Lower != NULL)
    {
        Lower->Driver->Chars.DevicePnPEventNotifyHandler(Lower->Context, NetDevicePnPEvent);
        CoreFilterRelease(Lower);
    }
    else if (Adapter->Core.Dispatch->DevicePnPEventNotifyHandler != NULL)
    {
        Adapter->Core.Dispatch->DevicePnPEventNotifyHandler(CORE_DISPATCH_CONTEXT(Adapter), NetDevicePnPEvent);
    }
}

_Use_decl_annotations_
VOID
NTAPI
NdisFPauseComplete(
    NDIS_HANDLE NdisFilterHandle)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    Module->OperationStatus = NDIS_STATUS_SUCCESS;
    KeSetEvent(&Module->OperationDone, IO_NO_INCREMENT, FALSE);
}

_Use_decl_annotations_
VOID
NTAPI
NdisFRestartComplete(
    NDIS_HANDLE NdisFilterHandle,
    NDIS_STATUS Status)
{
    PCORE_FILTER_MODULE Module = (PCORE_FILTER_MODULE)NdisFilterHandle;

    Module->OperationStatus = Status;
    KeSetEvent(&Module->OperationDone, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief
 * The Hyper-V switch extension handlers, which have no switch to serve here.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisFGetOptionalSwitchHandlers(
    NDIS_HANDLE NdisFilterHandle,
    PNDIS_SWITCH_CONTEXT NdisSwitchContext,
    struct _NDIS_SWITCH_OPTIONAL_HANDLERS *NdisSwitchHandlers)
{
    UNREFERENCED_PARAMETER(NdisFilterHandle);
    UNREFERENCED_PARAMETER(NdisSwitchHandlers);

    *NdisSwitchContext = NULL;
    return NDIS_STATUS_NOT_SUPPORTED;
}

/* EOF */
