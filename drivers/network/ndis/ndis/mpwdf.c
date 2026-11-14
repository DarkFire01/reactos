/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Miniports whose device objects a WDF class extension owns
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"
#include <ndiswdf.h>
#include <ndisguid.h>

/*
 * A class extension such as NetAdapterCx owns the FDO and all PnP and power
 * IRPs. It hands NDIS the device objects and the adapter block memory through
 * its callbacks, and drives the adapter through NdisWdfPnpPowerEventHandler.
 */

/* Driver object extension id of a class extension's registration */
#define CORE_WDF_CX_EXTENSION_ID    ((PVOID)'NWID')

typedef enum _CORE_WDF_CX_STATE
{
    CoreWdfCxRegistered = 1,
    CoreWdfCxDeregistered
} CORE_WDF_CX_STATE;

typedef struct _CORE_WDF_CX_DRIVER
{
    PDRIVER_OBJECT DriverObject;
    NDIS_WDF_CX_DRIVER_CONTEXT CxDriverContext;
    NDIS_WDF_CX_CHARACTERISTICS Characteristics;
    CORE_WDF_CX_STATE State;
} CORE_WDF_CX_DRIVER, *PCORE_WDF_CX_DRIVER;

/* A handle opened on an adapter through the class extension's device */
typedef struct _CORE_WDF_OPEN
{
    LIST_ENTRY ListEntry;
    PLOGICAL_ADAPTER Adapter;
    EX_RUNDOWN_REF Rundown;
    KEVENT RevokeDone;
    BOOLEAN Linked;
    BOOLEAN Revoking;
} CORE_WDF_OPEN, *PCORE_WDF_OPEN;

typedef struct _CORE_WDF_WORK
{
    PIO_WORKITEM WorkItem;
    PLOGICAL_ADAPTER Adapter;
} CORE_WDF_WORK, *PCORE_WDF_WORK;

/* Guards every adapter's OpenList and the state of every open */
static KSPIN_LOCK CoreWdfOpenLock;

/* Keywords of legacy hardware setup that a class extension's miniport never reads */
static const PCWSTR CoreWdfRetiredKeywords[] =
{
    L"Environment",
    L"ProcessorType",
    L"NdisVersion",
    L"MiniportName",
    L"BusType",
    L"UpperBindings",
    L"IoBaseAddress",
    L"IoAddress",
    L"IOBase",
    L"InterruptNumber",
    L"Interrupt",
    L"Irq",
    L"MemoryMappedBaseAddress",
    L"RamAddress",
    L"DmaChannel",
};

#define CORE_WDF_CX(_Adapter) (&(_Adapter)->Wdf.CxDriver->Characteristics)

static IO_WORKITEM_ROUTINE CoreWdfApplyWorker;

/* State lock */

static
VOID
CoreWdfLock(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    KeWaitForSingleObject(&Adapter->Wdf.StateLock, Executive, KernelMode, FALSE, NULL);
}

static
VOID
CoreWdfUnlock(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    KeSetEvent(&Adapter->Wdf.StateLock, IO_NO_INCREMENT, FALSE);
}

/* Registration */

/**
 * @brief
 * Registers a WDF class extension with NDIS.
 *
 * @param[in] DriverObject
 * The class extension's driver object.
 *
 * @param[in] RegistryPath
 * Its service key.
 *
 * @param[in] CxDriverContext
 * The class extension's own context.
 *
 * @param[in] CxDriverCharacteristics
 * The callbacks NDIS uses to reach the class extension.
 *
 * @param[out] NdisCxDriverHandle
 * The registration, for NdisWdfRegisterMiniportDriver.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INFO_LENGTH_MISMATCH for characteristics of another
 * size, or why the registration could not be stored.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfRegisterCx(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath,
    NDIS_WDF_CX_DRIVER_CONTEXT CxDriverContext,
    PNDIS_WDF_CX_CHARACTERISTICS CxDriverCharacteristics,
    NDIS_WDF_CX_DRIVER *NdisCxDriverHandle)
{
    PCORE_WDF_CX_DRIVER CxDriver;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    if (CxDriverCharacteristics->Header.Size != sizeof(*CxDriverCharacteristics))
        return STATUS_INFO_LENGTH_MISMATCH;

    Status = IoAllocateDriverObjectExtension(DriverObject,
                                             CORE_WDF_CX_EXTENSION_ID,
                                             sizeof(*CxDriver),
                                             (PVOID *)&CxDriver);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(CxDriver, sizeof(*CxDriver));
    CxDriver->DriverObject = DriverObject;
    CxDriver->CxDriverContext = CxDriverContext;
    RtlCopyMemory(&CxDriver->Characteristics, CxDriverCharacteristics, sizeof(CxDriver->Characteristics));
    CxDriver->State = CoreWdfCxRegistered;

    *NdisCxDriverHandle = (NDIS_WDF_CX_DRIVER)CxDriver;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Marks a class extension's registration as gone. The storage lives as long
 * as the driver object.
 *
 * @param[in] NdisCxDriverHandle
 * The registration.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfDeregisterCx(
    NDIS_WDF_CX_DRIVER NdisCxDriverHandle)
{
    ((PCORE_WDF_CX_DRIVER)NdisCxDriverHandle)->State = CoreWdfCxDeregistered;
}

/**
 * @brief
 * Registers a miniport driver on behalf of a class extension.
 *
 * @param[in] DriverObject
 * The client driver's driver object.
 *
 * @param[in] RegistryPath
 * The client driver's service key.
 *
 * @param[in] NdisCxDriverHandle
 * The class extension, from NdisWdfRegisterCx.
 *
 * @param[in] MiniportDriverContext
 * The context MiniportInitializeEx gets.
 *
 * @param[in] MiniportDriverCharacteristics
 * The class extension's miniport handlers.
 *
 * @param[out] NdisMiniportDriverHandle
 * The miniport driver.
 *
 * @return
 * The registration's status as an NTSTATUS.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfRegisterMiniportDriver(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath,
    NDIS_WDF_CX_DRIVER NdisCxDriverHandle,
    NDIS_HANDLE MiniportDriverContext,
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    PNDIS_HANDLE NdisMiniportDriverHandle)
{
    NDIS_STATUS NdisStatus;

    NdisStatus = NdisMRegisterMiniportDriver(DriverObject,
                                             RegistryPath,
                                             MiniportDriverContext,
                                             MiniportDriverCharacteristics,
                                             NdisMiniportDriverHandle);
    if (NdisStatus != NDIS_STATUS_SUCCESS)
        return NdisConvertNdisStatusToNtStatus(NdisStatus);

    ((PNDIS_M_DRIVER_BLOCK)*NdisMiniportDriverHandle)->CxDriver = (PCORE_WDF_CX_DRIVER)NdisCxDriverHandle;
    return STATUS_SUCCESS;
}

/* Adding an adapter */

static
NTSTATUS
CoreWdfReadString(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _Out_ PUNICODE_STRING String)
{
    PKEY_VALUE_PARTIAL_INFORMATION Value;
    UNICODE_STRING ValueName;
    ULONG Length;
    ULONG Chars;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);
    RtlInitEmptyUnicodeString(String, NULL, 0);

    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, NULL, 0, &Length);
    if (Status != STATUS_BUFFER_TOO_SMALL && Status != STATUS_BUFFER_OVERFLOW)
        return NT_SUCCESS(Status) ? STATUS_OBJECT_NAME_NOT_FOUND : Status;

    Value = ExAllocatePoolWithTag(PagedPool, Length, NDIS_TAG);
    if (Value == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, Length, &Length);
    if (NT_SUCCESS(Status) &&
        Value->Type != REG_SZ && Value->Type != REG_EXPAND_SZ && Value->Type != REG_MULTI_SZ)
    {
        Status = STATUS_OBJECT_TYPE_MISMATCH;
    }

    if (NT_SUCCESS(Status))
    {
        /* A multi string gives its first string */
        Chars = 0;
        while (Chars < Value->DataLength / sizeof(WCHAR) && ((PWCH)Value->Data)[Chars] != UNICODE_NULL)
            Chars++;

        String->Buffer = ExAllocatePoolWithTag(NonPagedPool, (Chars + 1) * sizeof(WCHAR), NDIS_TAG);
        if (String->Buffer != NULL)
        {
            RtlCopyMemory(String->Buffer, Value->Data, Chars * sizeof(WCHAR));
            String->Buffer[Chars] = UNICODE_NULL;
            String->Length = (USHORT)(Chars * sizeof(WCHAR));
            String->MaximumLength = (USHORT)((Chars + 1) * sizeof(WCHAR));
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    ExFreePoolWithTag(Value, NDIS_TAG);
    return Status;
}

static
VOID
CoreWdfFreeString(
    _Inout_ PUNICODE_STRING String)
{
    if (String->Buffer != NULL)
        ExFreePoolWithTag(String->Buffer, NDIS_TAG);

    RtlInitEmptyUnicodeString(String, NULL, 0);
}

/* The adapter's device name, \Device\{GUID}, from the driver key's Linkage */
static
NTSTATUS
CoreWdfReadExportName(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PUNICODE_STRING ExportName)
{
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING LinkageName = RTL_CONSTANT_STRING(L"Linkage");
    HANDLE DriverKey;
    HANDLE LinkageKey;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(ExportName, NULL, 0);

    Status = IoOpenDeviceRegistryKey(PhysicalDeviceObject, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &DriverKey);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&Attributes, &LinkageName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, DriverKey, NULL);
    Status = ZwOpenKey(&LinkageKey, KEY_READ, &Attributes);
    ZwClose(DriverKey);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = CoreWdfReadString(LinkageKey, L"Export", ExportName);
    ZwClose(LinkageKey);

    return Status;
}

/* The name WMI and the class extension know the adapter by */
static
NTSTATUS
CoreWdfQueryInstanceName(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ PCUNICODE_STRING Fallback,
    _Out_ PUNICODE_STRING InstanceName)
{
    static const DEVICE_REGISTRY_PROPERTY Properties[] =
    {
        DevicePropertyFriendlyName,
        DevicePropertyDeviceDescription,
    };
    ULONG Length;
    ULONG i;
    PWCH Buffer;
    NTSTATUS Status;

    for (i = 0; i < RTL_NUMBER_OF(Properties); i++)
    {
        Length = 0;
        Status = IoGetDeviceProperty(PhysicalDeviceObject, Properties[i], 0, NULL, &Length);
        if (Status != STATUS_BUFFER_TOO_SMALL || Length <= sizeof(WCHAR))
            continue;

        Buffer = ExAllocatePoolWithTag(NonPagedPool, Length, NDIS_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = IoGetDeviceProperty(PhysicalDeviceObject, Properties[i], Length, Buffer, &Length);
        if (NT_SUCCESS(Status))
        {
            InstanceName->Buffer = Buffer;
            InstanceName->MaximumLength = (USHORT)Length;
            InstanceName->Length = 0;
            while (InstanceName->Length < Length - sizeof(WCHAR) &&
                   Buffer[InstanceName->Length / sizeof(WCHAR)] != UNICODE_NULL)
            {
                InstanceName->Length += sizeof(WCHAR);
            }
            return STATUS_SUCCESS;
        }

        ExFreePoolWithTag(Buffer, NDIS_TAG);
    }

    InstanceName->Buffer = ExAllocatePoolWithTag(NonPagedPool, Fallback->Length + sizeof(WCHAR), NDIS_TAG);
    if (InstanceName->Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    InstanceName->Length = 0;
    InstanceName->MaximumLength = Fallback->Length + sizeof(WCHAR);
    RtlCopyUnicodeString(InstanceName, Fallback);
    return STATUS_SUCCESS;
}

/* The file name of the client driver's image, from its service key */
static
NTSTATUS
CoreWdfQueryImageName(
    _In_ PNDIS_M_DRIVER_BLOCK Driver,
    _Out_ PUNICODE_STRING ImageName)
{
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING ImagePath;
    UNICODE_STRING Service;
    HANDLE Key;
    USHORT Start;

    RtlInitEmptyUnicodeString(&ImagePath, NULL, 0);

    InitializeObjectAttributes(&Attributes,
                               Driver->RegistryPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Attributes)))
    {
        CoreWdfReadString(Key, L"ImagePath", &ImagePath);
        ZwClose(Key);
    }

    /* Without an image path the service name stands in */
    if (ImagePath.Buffer == NULL)
    {
        Service = *Driver->RegistryPath;

        ImagePath.Buffer = ExAllocatePoolWithTag(NonPagedPool, Service.Length + 5 * sizeof(WCHAR), NDIS_TAG);
        if (ImagePath.Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        ImagePath.Length = 0;
        ImagePath.MaximumLength = Service.Length + 5 * sizeof(WCHAR);
        RtlAppendUnicodeStringToString(&ImagePath, &Service);
        RtlAppendUnicodeToString(&ImagePath, L".sys");
    }

    for (Start = ImagePath.Length / sizeof(WCHAR); Start > 0; Start--)
    {
        if (ImagePath.Buffer[Start - 1] == L'\\')
            break;
    }

    ImageName->Length = ImagePath.Length - Start * sizeof(WCHAR);
    ImageName->MaximumLength = ImageName->Length + sizeof(WCHAR);
    ImageName->Buffer = ExAllocatePoolWithTag(NonPagedPool, ImageName->MaximumLength, NDIS_TAG);
    if (ImageName->Buffer == NULL)
    {
        CoreWdfFreeString(&ImagePath);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(ImageName->Buffer, &ImagePath.Buffer[Start], ImageName->Length);
    ImageName->Buffer[ImageName->Length / sizeof(WCHAR)] = UNICODE_NULL;

    CoreWdfFreeString(&ImagePath);
    return STATUS_SUCCESS;
}

/* The device name only links to the interface once there is one */
static
VOID
CoreWdfFreeNames(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    if (Adapter->Wdf.InterfaceLink.Buffer != NULL)
    {
        IoDeleteSymbolicLink(&Adapter->NdisMiniportBlock.MiniportName);
        RtlFreeUnicodeString(&Adapter->Wdf.InterfaceLink);
    }

    CoreWdfFreeString(&Adapter->Wdf.InstanceName);
    CoreWdfFreeString(&Adapter->Wdf.DriverImageName);
    CoreWdfFreeString(&Adapter->NdisMiniportBlock.MiniportName);
    RtlInitEmptyUnicodeString(&Adapter->Wdf.BaseName, NULL, 0);
}

static
VOID
CoreWdfInitializeKnobs(
    _Out_ PCORE_WDF_EC_KNOBS Knobs)
{
    ULONG i;

    RtlZeroMemory(Knobs, sizeof(*Knobs));
    Knobs->Size = sizeof(*Knobs);

    /* The first loop of a new execution context runs in a DPC */
    Knobs->Flags = 1;
    Knobs->DispatchTimeWarningInterval = 900000;
    Knobs->DpcWatchdogTimerThreshold = 80;
    Knobs->WorkerThreadPriority = 10;

    for (i = 0; i < 2; i++)
    {
        Knobs->MaxPacketsSend[i] = 64;
        Knobs->MaxPacketsSendComplete[i] = 64;
        Knobs->MaxPacketsReceive[i] = 64;
        Knobs->MaxPacketsReceiveComplete[i] = 64;
    }
}

/**
 * @brief
 * Creates the NDIS side of an adapter a class extension is adding. The block
 * lives in memory the class extension gives out, next to its own adapter.
 *
 * @param[in] AddDeviceInfo
 * The client driver, the PDO and the class extension's adapter context.
 *
 * @param[out] NdisAdapterHandle
 * The adapter, for the rest of the NdisWdf calls.
 *
 * @return
 * STATUS_SUCCESS, or why the adapter could not be added.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfPnPAddDevice(
    PNDIS_WDF_ADD_DEVICE_INFO AddDeviceInfo,
    PNDIS_HANDLE NdisAdapterHandle)
{
    PNDIS_WDF_CX_CHARACTERISTICS CxChars;
    PNDIS_M_DRIVER_BLOCK *MiniportPtr;
    PNDIS_M_DRIVER_BLOCK Miniport;
    NDIS_WDF_COMPLETE_ADD_PARAMS Params;
    UNICODE_STRING ExportName;
    PLOGICAL_ADAPTER Adapter;
    HANDLE Key;
    USHORT Start;
    NTSTATUS Status;

    *NdisAdapterHandle = NULL;

    MiniportPtr = IoGetDriverObjectExtension(AddDeviceInfo->DriverObject, (PVOID)'NMID');
    if (MiniportPtr == NULL || *MiniportPtr == NULL || (*MiniportPtr)->CxDriver == NULL)
        return STATUS_UNSUCCESSFUL;

    Miniport = *MiniportPtr;
    CxChars = &Miniport->CxDriver->Characteristics;

    Status = CoreWdfReadExportName(AddDeviceInfo->PhysicalDeviceObject, &ExportName);
    if (!NT_SUCCESS(Status))
    {
        NDIS_DbgPrint(MIN_TRACE, ("No device name for the adapter (0x%lx).\n", Status));
        return Status;
    }

    Status = CxChars->EvtCxAllocateMiniportBlock(AddDeviceInfo->MiniportAdapterContext,
                                                 sizeof(LOGICAL_ADAPTER),
                                                 (PVOID *)&Adapter);
    if (!NT_SUCCESS(Status))
    {
        CoreWdfFreeString(&ExportName);
        return Status;
    }

    RtlZeroMemory(Adapter, sizeof(*Adapter));
    KeInitializeSpinLock(&Adapter->NdisMiniportBlock.Lock);
    InitializeListHead(&Adapter->ProtocolListHead);
    CoreInitializeAdapterBlock(Adapter);

    /* The data path waits for the class extension to start it */
    Adapter->Core.PauseReasons = CORE_PAUSE_WDF;

    Adapter->Wdf.CxDriver = Miniport->CxDriver;
    Adapter->Wdf.CxAdapter = AddDeviceInfo->MiniportAdapterContext;
    ExInitializeRundownProtection(&Adapter->Wdf.Rundown);
    KeInitializeEvent(&Adapter->Wdf.StateLock, SynchronizationEvent, TRUE);
    InitializeListHead(&Adapter->Wdf.OpenList);
    CoreWdfInitializeKnobs(&Adapter->Wdf.Knobs);

    Adapter->NdisMiniportBlock.DriverHandle = Miniport;
    Adapter->NdisMiniportBlock.MiniportName = ExportName;
    Adapter->NdisMiniportBlock.MiniportAdapterContext = AddDeviceInfo->MiniportAdapterContext;
    Adapter->NdisMiniportBlock.PhysicalDeviceObject = AddDeviceInfo->PhysicalDeviceObject;
    Adapter->NdisMiniportBlock.DeviceObject = CxChars->EvtCxGetDeviceObject(Adapter->Wdf.CxAdapter);
    Adapter->NdisMiniportBlock.NextDeviceObject = CxChars->EvtCxGetNextDeviceObject(Adapter->Wdf.CxAdapter);
    Adapter->NdisMiniportBlock.OldPnPDeviceState = 0;
    Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceAdded;

    KeInitializeTimer(&Adapter->NdisMiniportBlock.WakeUpDpcTimer.Timer);

    /* The base name is the device name past \Device\ */
    for (Start = ExportName.Length / sizeof(WCHAR); Start > 0; Start--)
    {
        if (ExportName.Buffer[Start - 1] == L'\\')
            break;
    }

    Adapter->Wdf.BaseName.Buffer = &ExportName.Buffer[Start];
    Adapter->Wdf.BaseName.Length = ExportName.Length - Start * sizeof(WCHAR);
    Adapter->Wdf.BaseName.MaximumLength = Adapter->Wdf.BaseName.Length + sizeof(WCHAR);

    Status = CoreWdfQueryInstanceName(AddDeviceInfo->PhysicalDeviceObject,
                                      &Adapter->Wdf.BaseName,
                                      &Adapter->Wdf.InstanceName);
    if (NT_SUCCESS(Status))
        Status = CoreWdfQueryImageName(Miniport, &Adapter->Wdf.DriverImageName);
    if (!NT_SUCCESS(Status))
        goto Fail;

    /* Opens of \Device\{GUID} reach the class extension's device with the base name as file name */
    Status = IoRegisterDeviceInterface(AddDeviceInfo->PhysicalDeviceObject,
                                       &GUID_DEVINTERFACE_NET,
                                       &Adapter->Wdf.BaseName,
                                       &Adapter->Wdf.InterfaceLink);
    if (!NT_SUCCESS(Status))
    {
        RtlInitEmptyUnicodeString(&Adapter->Wdf.InterfaceLink, NULL, 0);
        goto Fail;
    }

    Status = IoCreateSymbolicLink(&Adapter->NdisMiniportBlock.MiniportName, &Adapter->Wdf.InterfaceLink);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeUnicodeString(&Adapter->Wdf.InterfaceLink);
        RtlInitEmptyUnicodeString(&Adapter->Wdf.InterfaceLink, NULL, 0);
        goto Fail;
    }

    Status = CoreRegisterInterface(Adapter);
    if (!NT_SUCCESS(Status))
        goto Fail;

    if (Miniport->PnpCharacteristics.MiniportAddDeviceHandler != NULL)
    {
        Status = Miniport->PnpCharacteristics.MiniportAddDeviceHandler(Adapter, Miniport->MiniportDriverContext);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            NDIS_DbgPrint(MIN_TRACE, ("MiniportAddDevice failed (0x%x).\n", Status));
            CoreDeregisterInterface(Adapter);
            Status = NdisConvertNdisStatusToNtStatus(Status);
            goto Fail;
        }
    }

    RtlZeroMemory(&Params, sizeof(Params));
    Params.InterfaceGuid = Adapter->Interface.InterfaceGuid;
    Params.NetLuid = Adapter->Interface.NetLuid;
    Params.MediaType = NdisMedium802_3;
    Params.BaseName = Adapter->Wdf.BaseName;
    Params.AdapterInstanceName = Adapter->Wdf.InstanceName;
    Params.DriverImageName = Adapter->Wdf.DriverImageName;
    Params.ExecutionContextKnobs = &Adapter->Wdf.Knobs;

    if (NT_SUCCESS(IoOpenDeviceRegistryKey(AddDeviceInfo->PhysicalDeviceObject,
                                           PLUGPLAY_REGKEY_DRIVER,
                                           KEY_READ,
                                           &Key)))
    {
        Params.MediaType = (NDIS_MEDIUM)CoreReadKeyUlong(Key, L"*MediaType", NdisMedium802_3);
        ZwClose(Key);
    }

    CxChars->EvtCxMiniportCompleteAdd(Adapter->Wdf.CxAdapter, &Params);

    *NdisAdapterHandle = Adapter;
    return STATUS_SUCCESS;

Fail:
    CoreWdfFreeNames(Adapter);
    return Status;
}

/* Binding and the data path */

/* Caller holds the state lock */
static
VOID
CoreWdfApply(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PPROTOCOL_BINDING Protocol;
    PLIST_ENTRY Entry;
    NDIS_STATUS NdisStatus;

    if (Adapter->Wdf.Removed || Adapter->Core.State == CoreMiniportHalted)
        return;

    if (Adapter->Wdf.DataPathRunning)
    {
        NdisStatus = CoreReleasePaused(Adapter, CORE_PAUSE_WDF);
        if (NdisStatus != NDIS_STATUS_SUCCESS)
            NDIS_DbgPrint(MIN_TRACE, ("Restart for the class extension failed (0x%x).\n", NdisStatus));
    }

    /* Protocols only find adapters on the global list */
    if (Adapter->Wdf.Started && !Adapter->Wdf.Bound)
    {
        Adapter->Wdf.Bound = TRUE;
        ExInterlockedInsertTailList(&AdapterListHead, &Adapter->ListEntry, &AdapterListLock);

        for (Entry = ProtocolListHead.Flink; Entry != &ProtocolListHead; Entry = Entry->Flink)
        {
            Protocol = CONTAINING_RECORD(Entry, PROTOCOL_BINDING, ListEntry);
            ndisBindMiniportsToProtocol(&NdisStatus, Protocol);
        }
    }
}

static
VOID
NTAPI
CoreWdfApplyWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PCORE_WDF_WORK Work = Context;
    PLOGICAL_ADAPTER Adapter = Work->Adapter;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoFreeWorkItem(Work->WorkItem);
    ExFreePoolWithTag(Work, NDIS_TAG);

    CoreWdfLock(Adapter);
    InterlockedExchange(&Adapter->Wdf.ApplyQueued, 0);
    CoreWdfApply(Adapter);
    CoreWdfUnlock(Adapter);

    ExReleaseRundownProtection(&Adapter->Wdf.Rundown);
}

/* Brings binding and the data path in line with what was asked, off the caller's thread */
static
VOID
CoreWdfQueueApply(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PCORE_WDF_WORK Work;

    if (InterlockedCompareExchange(&Adapter->Wdf.ApplyQueued, 1, 0) != 0)
        return;

    if (!ExAcquireRundownProtection(&Adapter->Wdf.Rundown))
    {
        InterlockedExchange(&Adapter->Wdf.ApplyQueued, 0);
        return;
    }

    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), NDIS_TAG);
    if (Work != NULL)
    {
        Work->Adapter = Adapter;
        Work->WorkItem = IoAllocateWorkItem(Adapter->NdisMiniportBlock.DeviceObject);
        if (Work->WorkItem != NULL)
        {
            IoQueueWorkItem(Work->WorkItem, CoreWdfApplyWorker, DelayedWorkQueue, Work);
            return;
        }

        ExFreePoolWithTag(Work, NDIS_TAG);
    }

    NDIS_DbgPrint(MIN_TRACE, ("No work item to apply the adapter state.\n"));
    InterlockedExchange(&Adapter->Wdf.ApplyQueued, 0);
    ExReleaseRundownProtection(&Adapter->Wdf.Rundown);
}

/**
 * @brief
 * The class extension is ready for protocols to bind to the adapter.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfMiniportStarted(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    ASSERT(MINIPORT_IS_WDF(Adapter));

    Adapter->Wdf.Started = TRUE;
    CoreWdfQueueApply(Adapter);
}

/**
 * @brief
 * The class extension has its data path up, so the miniport can restart.
 * The restart happens after this returns.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfMiniportDataPathStart(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    InterlockedExchange(&Adapter->Wdf.DataPathRunning, TRUE);
    CoreWdfQueueApply(Adapter);
}

/* Caller holds the state lock */
static
VOID
CoreWdfPauseDataPath(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    InterlockedExchange(&Adapter->Wdf.DataPathRunning, FALSE);
    CoreHoldPaused(Adapter, CORE_PAUSE_WDF);
}

/**
 * @brief
 * Pauses the miniport before the class extension takes its data path down,
 * and returns once it is paused.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfMiniportDataPathPause(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    CoreWdfLock(Adapter);
    CoreWdfPauseDataPath(Adapter);
    CoreWdfUnlock(Adapter);
}

/* Handles */

/**
 * @brief
 * Ends every handle open on an adapter. Requests in flight on them finish
 * first, later ones fail with NDIS_STATUS_ADAPTER_REMOVED.
 *
 * @param[in] Adapter
 * The adapter being stopped or removed.
 */
VOID
NTAPI
CoreWdfRevokeOpens(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PCORE_WDF_OPEN Open;
    LIST_ENTRY Revoked;
    KIRQL OldIrql;

    InitializeListHead(&Revoked);

    KeAcquireSpinLock(&CoreWdfOpenLock, &OldIrql);
    Adapter->Wdf.OpensAllowed = FALSE;
    while (!IsListEmpty(&Adapter->Wdf.OpenList))
    {
        Open = CONTAINING_RECORD(RemoveHeadList(&Adapter->Wdf.OpenList), CORE_WDF_OPEN, ListEntry);
        Open->Linked = FALSE;
        Open->Revoking = TRUE;
        InsertTailList(&Revoked, &Open->ListEntry);
    }
    KeReleaseSpinLock(&CoreWdfOpenLock, OldIrql);

    while (!IsListEmpty(&Revoked))
    {
        Open = CONTAINING_RECORD(RemoveHeadList(&Revoked), CORE_WDF_OPEN, ListEntry);
        ExWaitForRundownProtectionRelease(&Open->Rundown);

        /* A close waiting on this frees the open as soon as the event is set */
        KeAcquireSpinLock(&CoreWdfOpenLock, &OldIrql);
        Open->Revoking = FALSE;
        KeSetEvent(&Open->RevokeDone, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&CoreWdfOpenLock, OldIrql);
    }
}

/**
 * @brief
 * Opens a handle on an adapter for the class extension, and completes the IRP.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter the file name named.
 *
 * @param[in] Irp
 * The IRP_MJ_CREATE.
 *
 * @return
 * The status the IRP was completed with.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfCreateIrpHandler(
    NDIS_HANDLE MiniportAdapterHandle,
    PIRP Irp)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PCORE_WDF_OPEN Open = NULL;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (Stack->FileObject == NULL)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Complete;
    }

    /* User mode queries are checked against the miniport's OIDs, so it needs some */
    if (Adapter->Core.SupportedOidList == NULL && Irp->RequestorMode == UserMode)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Complete;
    }

    Open = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Open), NDIS_TAG);
    if (Open == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Complete;
    }

    RtlZeroMemory(Open, sizeof(*Open));
    Open->Adapter = Adapter;
    ExInitializeRundownProtection(&Open->Rundown);
    KeInitializeEvent(&Open->RevokeDone, NotificationEvent, FALSE);

    KeAcquireSpinLock(&CoreWdfOpenLock, &OldIrql);
    if (Adapter->Wdf.OpensAllowed)
    {
        InsertTailList(&Adapter->Wdf.OpenList, &Open->ListEntry);
        Open->Linked = TRUE;
        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    KeReleaseSpinLock(&CoreWdfOpenLock, OldIrql);

    if (NT_SUCCESS(Status))
    {
        Stack->FileObject->FsContext = Open;
        Open = NULL;
    }

Complete:
    if (Open != NULL)
        ExFreePoolWithTag(Open, NDIS_TAG);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/**
 * @brief
 * Closes a handle from NdisWdfCreateIrpHandler, and completes the IRP.
 *
 * @param[in] Irp
 * The IRP_MJ_CLOSE.
 *
 * @return
 * STATUS_SUCCESS.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfCloseIrpHandler(
    PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PCORE_WDF_OPEN Open = Stack->FileObject->FsContext;
    BOOLEAN Wait = FALSE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&CoreWdfOpenLock, &OldIrql);
    if (Open->Linked)
    {
        RemoveEntryList(&Open->ListEntry);
        Open->Linked = FALSE;
    }
    else
    {
        Wait = Open->Revoking;
    }
    KeReleaseSpinLock(&CoreWdfOpenLock, OldIrql);

    if (Wait)
        KeWaitForSingleObject(&Open->RevokeDone, Executive, KernelMode, FALSE, NULL);

    ExFreePoolWithTag(Open, NDIS_TAG);
    Stack->FileObject->FsContext = NULL;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
CoreWdfFailRemoved(
    _In_ PIRP Irp)
{
    Irp->IoStatus.Status = NDIS_STATUS_ADAPTER_REMOVED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return NDIS_STATUS_ADAPTER_REMOVED;
}

/**
 * @brief
 * Handles an IRP_MJ_DEVICE_CONTROL on a handle from NdisWdfCreateIrpHandler.
 *
 * @param[in] Irp
 * The request.
 *
 * @return
 * Its status.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfDeviceControlIrpHandler(
    PIRP Irp)
{
    PCORE_WDF_OPEN Open = IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext;
    NTSTATUS Status;

    if (!ExAcquireRundownProtection(&Open->Rundown))
        return CoreWdfFailRemoved(Irp);

    Status = MiniDeviceIoControl(Open->Adapter, Irp);
    ExReleaseRundownProtection(&Open->Rundown);

    return Status;
}

/**
 * @brief
 * Passes an IRP_MJ_INTERNAL_DEVICE_CONTROL on a handle from
 * NdisWdfCreateIrpHandler down the device stack.
 *
 * @param[in] Irp
 * The request.
 *
 * @return
 * Its status.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfDeviceInternalControlIrpHandler(
    PIRP Irp)
{
    PCORE_WDF_OPEN Open = IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext;
    PDEVICE_OBJECT NextDevice;
    NTSTATUS Status;

    if (!ExAcquireRundownProtection(&Open->Rundown))
        return CoreWdfFailRemoved(Irp);

    NextDevice = Open->Adapter->NdisMiniportBlock.NextDeviceObject;
    if (NextDevice != NULL)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        Status = IoCallDriver(NextDevice, Irp);
    }
    else
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    ExReleaseRundownProtection(&Open->Rundown);
    return Status;
}

/* PnP and power */

static
NTSTATUS
CoreWdfStart(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    NDIS_WRAPPER_CONTEXT WrapperContext;
    NDIS_STATUS NdisStatus;
    NTSTATUS Status;
    KIRQL OldIrql;

    RtlZeroMemory(&WrapperContext, sizeof(WrapperContext));
    WrapperContext.DeviceObject = Adapter->NdisMiniportBlock.DeviceObject;

    Status = IoOpenDeviceRegistryKey(Adapter->NdisMiniportBlock.PhysicalDeviceObject,
                                     PLUGPLAY_REGKEY_DRIVER,
                                     KEY_ALL_ACCESS,
                                     &WrapperContext.RegistryHandle);
    if (!NT_SUCCESS(Status))
        return STATUS_UNSUCCESSFUL;

    CoreWdfLock(Adapter);

    /* MiniportInitializeEx runs on this thread, and leaves the data path paused */
    NdisStatus = CoreInitializeAdapter(Adapter, &WrapperContext);
    ZwClose(WrapperContext.RegistryHandle);

    if (NdisStatus != NDIS_STATUS_SUCCESS)
    {
        CoreWdfUnlock(Adapter);
        NDIS_DbgPrint(MIN_TRACE, ("MiniportInitializeEx failed (0x%x).\n", NdisStatus));
        return STATUS_UNSUCCESSFUL;
    }

    Adapter->NdisMiniportBlock.OldPnPDeviceState = Adapter->NdisMiniportBlock.PnPDeviceState;
    Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceStarted;

    ExInterlockedInsertTailList(&Adapter->NdisMiniportBlock.DriverHandle->DeviceList,
                                &Adapter->MiniportListEntry,
                                &Adapter->NdisMiniportBlock.DriverHandle->Lock);

    IoSetDeviceInterfaceState(&Adapter->Wdf.InterfaceLink, TRUE);

    KeAcquireSpinLock(&CoreWdfOpenLock, &OldIrql);
    Adapter->Wdf.OpensAllowed = TRUE;
    KeReleaseSpinLock(&CoreWdfOpenLock, OldIrql);

    /* A restart after a stop picks up where the class extension left the data path */
    CoreWdfApply(Adapter);

    CoreWdfUnlock(Adapter);
    return STATUS_SUCCESS;
}

/* Caller holds the state lock */
static
VOID
CoreWdfHalt(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    if (Adapter->Wdf.Bound)
    {
        ExInterlockedRemoveEntryList(&Adapter->ListEntry, &AdapterListLock);
        Adapter->Wdf.Bound = FALSE;
    }

    CoreWdfRevokeOpens(Adapter);

    if (Adapter->Core.State == CoreMiniportHalted)
        return;

    ExInterlockedRemoveEntryList(&Adapter->MiniportListEntry, &Adapter->NdisMiniportBlock.DriverHandle->Lock);
    IoSetDeviceInterfaceState(&Adapter->Wdf.InterfaceLink, FALSE);

    CoreHaltAdapter(Adapter, HaltAction);
}

static
NTSTATUS
CoreWdfStop(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    CoreWdfLock(Adapter);

    Adapter->NdisMiniportBlock.OldPnPDeviceState = Adapter->NdisMiniportBlock.PnPDeviceState;
    Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceQueryStopped;

    CoreWdfHalt(Adapter, NdisHaltDeviceStopped);
    Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceStopped;

    CoreWdfPauseDataPath(Adapter);

    CoreWdfUnlock(Adapter);
    return STATUS_SUCCESS;
}

static
NTSTATUS
CoreWdfSurpriseRemove(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    NET_DEVICE_PNP_EVENT DeviceEvent;
    NET_PNP_EVENT PnPEvent;

    CoreWdfLock(Adapter);

    if (Adapter->Core.State != CoreMiniportHalted &&
        Adapter->NdisMiniportBlock.PnPDeviceState == NdisPnPDeviceStarted)
    {
        RtlZeroMemory(&DeviceEvent, sizeof(DeviceEvent));
        DeviceEvent.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        DeviceEvent.Header.Revision = NET_DEVICE_PNP_EVENT_REVISION_1;
        DeviceEvent.Header.Size = NDIS_SIZEOF_NET_DEVICE_PNP_EVENT_REVISION_1;
        DeviceEvent.PortNumber = NDIS_DEFAULT_PORT_NUMBER;
        DeviceEvent.DevicePnPEvent = NdisDevicePnPEventSurpriseRemoved;

        Adapter->Core.Dispatch->DevicePnPEventNotifyHandler(CORE_DISPATCH_CONTEXT(Adapter), &DeviceEvent);
    }

    Adapter->NdisMiniportBlock.OldPnPDeviceState = Adapter->NdisMiniportBlock.PnPDeviceState;
    Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceRemoved;

    /* The protocols hear the device is going before the miniport halts */
    RtlZeroMemory(&PnPEvent, sizeof(PnPEvent));
    PnPEvent.NetEvent = NetEventQueryRemoveDevice;
    CoreNotifyProtocols(Adapter, &PnPEvent);

    CoreWdfHalt(Adapter, NdisHaltDeviceSurpriseRemoved);

    CoreWdfUnlock(Adapter);
    return STATUS_SUCCESS;
}

static
NTSTATUS
CoreWdfRemove(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;

    CoreWdfLock(Adapter);
    CoreWdfHalt(Adapter, NdisHaltDeviceDisabled);
    Adapter->Wdf.Removed = TRUE;
    Adapter->NdisMiniportBlock.OldPnPDeviceState = Adapter->NdisMiniportBlock.PnPDeviceState;
    Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceRemoved;
    CoreWdfUnlock(Adapter);

    /* Every reference, and any queued state work, has to drain before the block goes */
    ExWaitForRundownProtectionRelease(&Adapter->Wdf.Rundown);

    if (Driver->PnpCharacteristics.MiniportRemoveDeviceHandler != NULL)
        Driver->PnpCharacteristics.MiniportRemoveDeviceHandler(Adapter->Core.AddDeviceContext);

    if (Adapter->NdisMiniportBlock.EthDB != NULL)
    {
        EthDeleteFilter(Adapter->NdisMiniportBlock.EthDB);
        Adapter->NdisMiniportBlock.EthDB = NULL;
    }

    CoreDeregisterInterface(Adapter);
    CoreWdfFreeNames(Adapter);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * The class extension moves the adapter through PnP and power.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] PnpPowerAction
 * What happens.
 *
 * @param[in] PowerAction
 * Not used.
 *
 * @return
 * STATUS_SUCCESS, STATUS_UNSUCCESSFUL for a start that failed,
 * STATUS_NOT_IMPLEMENTED to stop power management, or
 * STATUS_INVALID_PARAMETER for an action NDIS does not take this way.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
NdisWdfPnpPowerEventHandler(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_WDF_PNP_POWER_ACTION PnpPowerAction,
    NDIS_WDF_PNP_POWER_ACTION PowerAction)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    UNREFERENCED_PARAMETER(PowerAction);

    ASSERT(MINIPORT_IS_WDF(Adapter));

    switch (PnpPowerAction)
    {
        case NdisWdfActionPnpStart:
            return CoreWdfStart(Adapter);

        case NdisWdfActionPnpStop:
            return CoreWdfStop(Adapter);

        case NdisWdfActionPnpSurpriseRemove:
            return CoreWdfSurpriseRemove(Adapter);

        case NdisWdfActionPnpRemove:
        case NdisWdfActionPreReleaseHardware:
        case NdisWdfActionPostReleaseHardware:
        case NdisWdfActionDeviceObjectCleanup:
            /* The older contract removes in one step, the newer one in three */
            if (PnpPowerAction == NdisWdfActionPnpRemove ||
                PnpPowerAction == NdisWdfActionDeviceObjectCleanup)
            {
                return Adapter->Wdf.Removed ? STATUS_SUCCESS : CoreWdfRemove(Adapter);
            }

            if (PnpPowerAction == NdisWdfActionPreReleaseHardware)
            {
                CoreWdfLock(Adapter);
                CoreWdfHalt(Adapter, NdisHaltDeviceDisabled);
                CoreWdfUnlock(Adapter);
            }
            return STATUS_SUCCESS;

        case NdisWdfActionPnpRebalance:
            CoreWdfLock(Adapter);
            Adapter->NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceStopped;
            CoreWdfPauseDataPath(Adapter);
            CoreWdfUnlock(Adapter);
            return STATUS_SUCCESS;

        /* Wake parameters only matter to an idle engine, and there is none */
        case NdisWdfActionPowerDx:
        case NdisWdfActionPowerDxOnSystemSx:
        case NdisWdfActionStartPowerManagement:
            return STATUS_SUCCESS;

        case NdisWdfActionStopPowerManagement:
            return STATUS_NOT_IMPLEMENTED;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/**
 * @brief
 * The device changed power state. A system transition pauses the miniport
 * on the way down and restarts it on the way back. Runtime idle is the class
 * extension's business.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] SystemPowerAction
 * The system transition behind it, or PowerActionNone.
 *
 * @param[in] DevicePowerState
 * The device's new state.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfMiniportSetPower(
    NDIS_HANDLE MiniportAdapterHandle,
    POWER_ACTION SystemPowerAction,
    DEVICE_POWER_STATE DevicePowerState)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;
    NDIS_DEVICE_POWER_STATE PowerState;
    NET_PNP_EVENT PnPEvent;

    ASSERT(MINIPORT_IS_WDF(Adapter));

    if (SystemPowerAction == PowerActionNone)
        return;

    PowerState = (NDIS_DEVICE_POWER_STATE)DevicePowerState;

    RtlZeroMemory(&PnPEvent, sizeof(PnPEvent));
    PnPEvent.NetEvent = NetEventSetPower;
    PnPEvent.Buffer = &PowerState;
    PnPEvent.BufferLength = sizeof(PowerState);

    CoreWdfLock(Adapter);

    if (DevicePowerState == PowerDeviceD0)
    {
        Adapter->NdisMiniportBlock.CurrentDevicePowerState = PowerDeviceD0;
        CoreReleasePaused(Adapter, CORE_PAUSE_LOW_POWER);
        CoreNotifyProtocols(Adapter, &PnPEvent);
    }
    else
    {
        CoreNotifyProtocols(Adapter, &PnPEvent);
        CoreHoldPaused(Adapter, CORE_PAUSE_LOW_POWER);
        Adapter->NdisMiniportBlock.CurrentDevicePowerState = DevicePowerState;
    }

    CoreWdfUnlock(Adapter);
}

/**
 * @brief
 * An asynchronous power reference NDIS asked the class extension for has
 * completed. NDIS never asks for one, having no idle engine.
 *
 * @param[in] MiniportAdapterContext
 * The adapter.
 *
 * @param[in] StatusOfOperation
 * Whether the device reached D0.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfAsyncPowerReferenceCompleteNotification(
    NDIS_HANDLE MiniportAdapterContext,
    NTSTATUS StatusOfOperation)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);

    NDIS_DbgPrint(MID_TRACE, ("Power reference completed (0x%lx).\n", StatusOfOperation));
}

/* References and small queries */

/**
 * @brief
 * Takes a reference on an adapter unless it is being removed.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @return
 * TRUE when the reference was taken.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
NdisWdfMiniportTryReference(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    ASSERT(MINIPORT_IS_WDF(Adapter));

    return ExAcquireRundownProtection(&Adapter->Wdf.Rundown);
}

/**
 * @brief
 * Drops a reference from NdisWdfMiniportTryReference.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfMiniportDereference(
    NDIS_HANDLE MiniportAdapterHandle)
{
    PLOGICAL_ADAPTER Adapter = MiniportAdapterHandle;

    ASSERT(MINIPORT_IS_WDF(Adapter));

    ExReleaseRundownProtection(&Adapter->Wdf.Rundown);
}

/**
 * @brief
 * Returns the context the miniport registered for an adapter.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @return
 * The miniport's adapter context.
 */
_Use_decl_annotations_
PVOID
NTAPI
NdisWdfGetAdapterContextFromAdapterHandle(
    NDIS_HANDLE MiniportAdapterHandle)
{
    return ((PLOGICAL_ADAPTER)MiniportAdapterHandle)->NdisMiniportBlock.MiniportAdapterContext;
}

/**
 * @brief
 * NdisReadConfiguration for a class extension's miniport, which cannot read
 * the keywords of legacy hardware setup.
 *
 * @param[out] Status
 * NDIS_STATUS_FAILURE for such a keyword, otherwise as NdisReadConfiguration.
 *
 * @param[out] ParameterValue
 * The value read.
 *
 * @param[in] ConfigurationHandle
 * The open configuration.
 *
 * @param[in] Keyword
 * The value name.
 *
 * @param[in] ParameterType
 * The type wanted.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisWdfReadConfiguration(
    PNDIS_STATUS Status,
    PNDIS_CONFIGURATION_PARAMETER *ParameterValue,
    NDIS_HANDLE ConfigurationHandle,
    PUNICODE_STRING Keyword,
    NDIS_PARAMETER_TYPE ParameterType)
{
    UNICODE_STRING Retired;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(CoreWdfRetiredKeywords); i++)
    {
        RtlInitUnicodeString(&Retired, CoreWdfRetiredKeywords[i]);
        if (RtlEqualUnicodeString(Keyword, &Retired, TRUE))
        {
            *Status = NDIS_STATUS_FAILURE;
            return;
        }
    }

    NdisReadConfiguration(Status, ParameterValue, ConfigurationHandle, Keyword, ParameterType);
}
