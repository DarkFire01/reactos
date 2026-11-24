/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WMI data provider registration and data block queries
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <wmistr.h>
#include "wmip.h"

#define NDEBUG
#include <debug.h>

#define TAG_WMI_PROVIDER 'PimW'

/* First registration buffer, providers that need more say so */
#define WMIP_REGINFO_INITIAL_SIZE   0x2000

/* Static name indexes stay below this, matching what Windows accepts */
#define WMIP_STATIC_INDEX_LIMIT     999999

#define WMIP_PROVIDER_REMOVED       0x00000001
#define WMIP_PROVIDER_BLOCKED       0x00000002

/* Blocks registered with these flags never see query or set requests */
#define WMIP_NON_DATA_FLAGS         (WMIREG_FLAG_EVENT_ONLY_GUID | WMIREG_FLAG_TRACED_GUID)

/* ALIGN_UP_BY goes through ULONG_PTR, sizes here are summed in 64 bits */
#define WMIP_ALIGN(Value, Align)    (((ULONGLONG)(Value) + ((Align) - 1)) & ~((ULONGLONG)(Align) - 1))

/* Which switch requests a data block has been sent */
#define WMIP_BLOCK_EVENTS_ON        0x01
#define WMIP_BLOCK_COLLECTION_ON    0x02

/* Retries of an event reference whose target grew between two queries */
#define WMIP_EVENT_REFERENCE_TRIES  2

/* One device that called IoWMIRegistrationControl */
typedef struct _WMIP_PROVIDER
{
    LIST_ENTRY ProviderLink;
    LIST_ENTRY BlockListHead;
    LIST_ENTRY MofListHead;
    PDEVICE_OBJECT DeviceObject;
    ULONG ProviderId;
    LONG RefCount;
    ULONG IrpCount;
    ULONG Flags;
    BOOLEAN Registered;
    BOOLEAN WorkQueued;
    BOOLEAN WorkPending;
    KEVENT IrpsDrained;
    WORK_QUEUE_ITEM WorkItem;
} WMIP_PROVIDER, *PWMIP_PROVIDER;

/* One GUID a provider listed in its registration information */
typedef struct _WMIP_DATA_BLOCK
{
    LIST_ENTRY BlockLink;
    PWMIP_PROVIDER Provider;
    LONG RefCount;
    GUID Guid;
    ULONG RegFlags;
    ULONG InstanceCount;
    ULONG BaseIndex;
    PWSTR BaseName;
    PWSTR *StaticNames;
    ULONG NameSpace;
    PDEVICE_OBJECT Pdo;
    UCHAR Control;
} WMIP_DATA_BLOCK, *PWMIP_DATA_BLOCK;

/* A MOF resource named by some provider, shared by every provider naming it */
typedef struct _WMIP_MOF_RESOURCE
{
    LIST_ENTRY MofLink;
    LONG RefCount;
    PWSTR RegistryPath;
    PWSTR ResourceName;
} WMIP_MOF_RESOURCE, *PWMIP_MOF_RESOURCE;

typedef struct _WMIP_MOF_REFERENCE
{
    LIST_ENTRY ProviderMofLink;
    PWMIP_MOF_RESOURCE Resource;
} WMIP_MOF_REFERENCE, *PWMIP_MOF_REFERENCE;

static LIST_ENTRY WmipProviderListHead;
static LIST_ENTRY WmipMofListHead;
static FAST_MUTEX WmipProviderLock;

/* Also held while the provider list changes, so ids can be looked up at DISPATCH_LEVEL */
static KSPIN_LOCK WmipProviderIdLock;
static ULONG WmipLastProviderId;

static WORKER_THREAD_ROUTINE WmipRegInfoWorker;
static IO_COMPLETION_ROUTINE WmipIrpCompletion;

static
VOID
NTAPI
WmipControlNewBlocks(
    _Inout_ PWMIP_PROVIDER Provider);


/* FUNCTIONS *****************************************************************/

VOID
NTAPI
WmipInitializeProviders(
    VOID)
{
    InitializeListHead(&WmipProviderListHead);
    InitializeListHead(&WmipMofListHead);
    ExInitializeFastMutex(&WmipProviderLock);
    KeInitializeSpinLock(&WmipProviderIdLock);
}

static
VOID
WmipFreeDataBlock(
    _In_ PWMIP_DATA_BLOCK Block)
{
    if (Block->BaseName)
        ExFreePoolWithTag(Block->BaseName, TAG_WMI_PROVIDER);
    if (Block->StaticNames)
        ExFreePoolWithTag(Block->StaticNames, TAG_WMI_PROVIDER);
    ExFreePoolWithTag(Block, TAG_WMI_PROVIDER);
}

static
VOID
WmipDereferenceProvider(
    _In_ PWMIP_PROVIDER Provider)
{
    if (InterlockedDecrement(&Provider->RefCount) != 0)
        return;

    ASSERT(IsListEmpty(&Provider->BlockListHead));
    ASSERT(IsListEmpty(&Provider->MofListHead));
    ObDereferenceObject(Provider->DeviceObject);
    ExFreePoolWithTag(Provider, TAG_WMI_PROVIDER);
}

static
VOID
WmipDereferenceDataBlock(
    _In_ PWMIP_DATA_BLOCK Block)
{
    PWMIP_PROVIDER Provider;

    if (InterlockedDecrement(&Block->RefCount) != 0)
        return;

    /* Attached blocks keep their provider alive */
    Provider = Block->Provider;
    WmipFreeDataBlock(Block);
    if (Provider)
        WmipDereferenceProvider(Provider);
}

static
VOID
WmipReleaseBlockList(
    _Inout_ PLIST_ENTRY ListHead)
{
    PWMIP_DATA_BLOCK Block;
    PLIST_ENTRY Entry;

    while (!IsListEmpty(ListHead))
    {
        Entry = RemoveHeadList(ListHead);
        Block = CONTAINING_RECORD(Entry, WMIP_DATA_BLOCK, BlockLink);
        WmipDereferenceDataBlock(Block);
    }
}

static
VOID
WmipMoveBlockListLocked(
    _Inout_ PLIST_ENTRY From,
    _Inout_ PLIST_ENTRY To)
{
    PLIST_ENTRY Entry;

    while (!IsListEmpty(From))
    {
        Entry = RemoveHeadList(From);
        InsertTailList(To, Entry);
    }
}

static
PWMIP_PROVIDER
WmipFindProviderLocked(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PWMIP_PROVIDER Provider;
    PLIST_ENTRY Entry;

    for (Entry = WmipProviderListHead.Flink;
         Entry != &WmipProviderListHead;
         Entry = Entry->Flink)
    {
        Provider = CONTAINING_RECORD(Entry, WMIP_PROVIDER, ProviderLink);
        if (Provider->DeviceObject == DeviceObject)
            return Provider;
    }

    return NULL;
}

static
NTSTATUS
NTAPI
WmipIrpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Sends one IRP_MJ_SYSTEM_CONTROL request to the top of a provider's stack and waits for it.
 */
static
NTSTATUS
NTAPI
WmipSendIrp(
    _In_ PWMIP_PROVIDER Provider,
    _In_ UCHAR MinorFunction,
    _In_opt_ PVOID DataPath,
    _In_ ULONG BufferSize,
    _Inout_updates_bytes_(BufferSize) PVOID Buffer,
    _Out_ PULONG_PTR Information)
{
    PDEVICE_OBJECT TopDevice;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;

    PAGED_CODE();

    *Information = 0;

    /* A provider on its way out fails like an unknown one */
    ExAcquireFastMutex(&WmipProviderLock);
    if (Provider->Flags & (WMIP_PROVIDER_REMOVED | WMIP_PROVIDER_BLOCKED))
    {
        ExReleaseFastMutex(&WmipProviderLock);
        if ((MinorFunction == IRP_MN_QUERY_SINGLE_INSTANCE) ||
            (MinorFunction == IRP_MN_CHANGE_SINGLE_INSTANCE))
        {
            return STATUS_WMI_INSTANCE_NOT_FOUND;
        }
        return STATUS_UNSUCCESSFUL;
    }
    Provider->IrpCount++;
    ExReleaseFastMutex(&WmipProviderLock);

    TopDevice = IoGetAttachedDeviceReference(Provider->DeviceObject);
    Irp = IoAllocateIrp(TopDevice->StackSize, FALSE);
    if (!Irp)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        Irp->Tail.Overlay.Thread = PsGetCurrentThread();
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;

        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->MajorFunction = IRP_MJ_SYSTEM_CONTROL;
        Stack->MinorFunction = MinorFunction;
        Stack->Parameters.WMI.ProviderId = (ULONG_PTR)Provider->DeviceObject;
        Stack->Parameters.WMI.DataPath = DataPath;
        Stack->Parameters.WMI.BufferSize = BufferSize;
        Stack->Parameters.WMI.Buffer = Buffer;
        IoSetCompletionRoutine(Irp, WmipIrpCompletion, &Event, TRUE, TRUE, TRUE);

        if (IoCallDriver(TopDevice, Irp) == STATUS_PENDING)
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

        Status = Irp->IoStatus.Status;
        *Information = Irp->IoStatus.Information;
        IoFreeIrp(Irp);

        /* Nobody in the stack claimed the request */
        if (Status == STATUS_NOT_SUPPORTED)
            Status = STATUS_WMI_GUID_NOT_FOUND;
    }

    ObDereferenceObject(TopDevice);

    ExAcquireFastMutex(&WmipProviderLock);
    Provider->IrpCount--;
    if ((Provider->IrpCount == 0) && (Provider->Flags & WMIP_PROVIDER_REMOVED))
        KeSetEvent(&Provider->IrpsDrained, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&WmipProviderLock);

    return Status;
}

/**
 * @brief Fetches the registration information, preferring IRP_MN_REGINFO_EX over IRP_MN_REGINFO.
 */
static
NTSTATUS
NTAPI
WmipQueryRegInfo(
    _In_ PWMIP_PROVIDER Provider,
    _In_ ULONG_PTR DataPath,
    _Outptr_result_bytebuffer_(*InfoSize) PWMIREGINFOW *OutInfo,
    _Out_ PULONG InfoSize,
    _Out_ PBOOLEAN Extended)
{
    ULONG Size = WMIP_REGINFO_INITIAL_SIZE;
    ULONG_PTR Information;
    PWMIREGINFOW Info;
    NTSTATUS Status;
    ULONG Needed;

    *OutInfo = NULL;
    *InfoSize = 0;

    for (;;)
    {
        Info = ExAllocatePoolWithTag(NonPagedPool, Size, TAG_WMI_PROVIDER);
        if (!Info)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Info, Size);

        *Extended = TRUE;
        Status = WmipSendIrp(Provider, IRP_MN_REGINFO_EX, (PVOID)DataPath, Size, Info, &Information);
        if (!NT_SUCCESS(Status) && (Status != STATUS_BUFFER_TOO_SMALL))
        {
            *Extended = FALSE;
            Status = WmipSendIrp(Provider, IRP_MN_REGINFO, (PVOID)DataPath, Size, Info, &Information);
        }

        if ((Status != STATUS_BUFFER_TOO_SMALL) && (Information != sizeof(ULONG)))
            break;

        /* The provider left the size it wants in the first ULONG */
        Needed = Info->BufferSize;
        ExFreePoolWithTag(Info, TAG_WMI_PROVIDER);
        if (Needed <= Size)
            return STATUS_UNSUCCESSFUL;
        Size = Needed;
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Info, TAG_WMI_PROVIDER);
        return Status;
    }

    *OutInfo = Info;
    *InfoSize = (Information < Size) ? (ULONG)Information : Size;
    return STATUS_SUCCESS;
}

/* Reads the counted string at Offset, an offset of zero is a valid absent string. */
static
BOOLEAN
WmipGetRegInfoString(
    _In_reads_bytes_(InfoSize) PWMIREGINFOW Info,
    _In_ ULONG InfoSize,
    _In_ ULONG Offset,
    _Out_ PUNICODE_STRING String)
{
    USHORT Length;

    String->Length = 0;
    String->MaximumLength = 0;
    String->Buffer = NULL;

    if (Offset == 0)
        return TRUE;

    if ((Offset & 1) || (InfoSize < sizeof(USHORT)) || (Offset > InfoSize - sizeof(USHORT)))
        return FALSE;

    Length = *(PUSHORT)((PUCHAR)Info + Offset);
    if (Length > InfoSize - Offset - sizeof(USHORT))
        return FALSE;

    String->Length = Length & ~(sizeof(WCHAR) - 1);
    String->MaximumLength = String->Length;
    String->Buffer = (PWCH)((PUCHAR)Info + Offset + sizeof(USHORT));
    return TRUE;
}

static
PWSTR
WmipCaptureName(
    _In_ PCUNICODE_STRING Name,
    _In_ BOOLEAN AppendSeparator)
{
    SIZE_T Chars = Name->Length / sizeof(WCHAR);
    PWSTR Copy;

    Copy = ExAllocatePoolWithTag(PagedPool, (Chars + 2) * sizeof(WCHAR), TAG_WMI_PROVIDER);
    if (!Copy)
        return NULL;

    RtlCopyMemory(Copy, Name->Buffer, Chars * sizeof(WCHAR));
    if (AppendSeparator)
        Copy[Chars++] = L'_';
    Copy[Chars] = UNICODE_NULL;
    return Copy;
}

/* Copies Count back to back counted strings into one allocation of terminated strings. */
static
PWSTR *
WmipCaptureNameList(
    _In_reads_bytes_(InfoSize) PWMIREGINFOW Info,
    _In_ ULONG InfoSize,
    _In_ ULONG Offset,
    _In_ ULONG Count)
{
    UNICODE_STRING Name;
    ULONG Index, Cursor;
    SIZE_T Total;
    PWSTR *List;
    PWCH Chars;

    Total = 0;
    Cursor = Offset;
    for (Index = 0; Index < Count; Index++)
    {
        if (!WmipGetRegInfoString(Info, InfoSize, Cursor, &Name) || !Name.Buffer)
            return NULL;
        Total += Name.Length + sizeof(UNICODE_NULL);
        Cursor += sizeof(USHORT) + Name.Length;
    }

    /* Every name took at least two bytes of the buffer, so Count is small here */
    Total += (SIZE_T)Count * sizeof(*List);
    List = ExAllocatePoolWithTag(PagedPool, Total ? Total : sizeof(*List), TAG_WMI_PROVIDER);
    if (!List)
        return NULL;

    Chars = (PWCH)(List + Count);
    Cursor = Offset;
    for (Index = 0; Index < Count; Index++)
    {
        WmipGetRegInfoString(Info, InfoSize, Cursor, &Name);
        List[Index] = Chars;
        RtlCopyMemory(Chars, Name.Buffer, Name.Length);
        Chars += Name.Length / sizeof(WCHAR);
        *Chars++ = UNICODE_NULL;
        Cursor += sizeof(USHORT) + Name.Length;
    }

    return List;
}

/* A PDO stands for a base name made of its device instance path and an underscore. */
static
PWSTR
WmipPdoBaseName(
    _In_ PDEVICE_OBJECT Pdo)
{
    PDEVICE_NODE DeviceNode;

    DeviceNode = IopGetDeviceNode(Pdo);
    if (!DeviceNode || !DeviceNode->InstancePath.Length)
        return NULL;

    return WmipCaptureName(&DeviceNode->InstancePath, TRUE);
}

/* Space static names take in a WNODE_ALL_DATA, the same upper bound Windows reports. */
static
BOOLEAN
WmipComputeNameSpace(
    _Inout_ PWMIP_DATA_BLOCK Block)
{
    ULONGLONG Space = 0;
    ULONG Index;

    if (Block->BaseName)
    {
        /* Length prefix, base and a decimal index of up to seven digits */
        Space = (ULONGLONG)Block->InstanceCount *
                (sizeof(ULONG) + (wcslen(Block->BaseName) + 8) * sizeof(WCHAR));
    }
    else if (Block->StaticNames)
    {
        Space = (ULONGLONG)Block->InstanceCount * sizeof(ULONG);
        for (Index = 0; Index < Block->InstanceCount; Index++)
            Space += (wcslen(Block->StaticNames[Index]) + 2) * sizeof(WCHAR);
    }

    if (Space > MAXULONG / 2)
        return FALSE;

    Block->NameSpace = (ULONG)Space;
    return TRUE;
}

static
PWMIP_DATA_BLOCK
NTAPI
WmipCreateDataBlock(
    _In_reads_bytes_(InfoSize) PWMIREGINFOW Info,
    _In_ ULONG InfoSize,
    _In_ PWMIREGGUIDW RegGuid,
    _In_opt_ PDEVICE_OBJECT Pdo)
{
    PWMIP_DATA_BLOCK Block;
    UNICODE_STRING Name;

    Block = ExAllocatePoolWithTag(PagedPool, sizeof(*Block), TAG_WMI_PROVIDER);
    if (!Block)
        return NULL;

    RtlZeroMemory(Block, sizeof(*Block));
    Block->RefCount = 1;
    Block->Guid = RegGuid->Guid;
    Block->RegFlags = RegGuid->Flags;
    Block->InstanceCount = RegGuid->InstanceCount;

    /* Removal records only carry the GUID */
    if (Block->RegFlags & WMIREG_FLAG_REMOVE_GUID)
        return Block;

    if (Pdo)
    {
        Block->RegFlags &= ~WMIREG_FLAG_INSTANCE_LIST;
        Block->BaseName = WmipPdoBaseName(Pdo);
        if (Block->BaseName)
        {
            Block->RegFlags |= WMIREG_FLAG_INSTANCE_BASENAME;
            Block->Pdo = Pdo;
        }
    }

    if (Block->RegFlags & WMIREG_FLAG_INSTANCE_LIST)
    {
        Block->StaticNames = WmipCaptureNameList(Info,
                                                 InfoSize,
                                                 RegGuid->InstanceNameList,
                                                 Block->InstanceCount);
        if (!Block->StaticNames)
            goto Fail;
    }
    else if ((Block->RegFlags & WMIREG_FLAG_INSTANCE_BASENAME) && !Block->BaseName)
    {
        if (!WmipGetRegInfoString(Info, InfoSize, RegGuid->BaseNameOffset, &Name) || !Name.Buffer)
            goto Fail;

        Block->BaseName = WmipCaptureName(&Name, FALSE);
        if (!Block->BaseName)
            goto Fail;
    }

    if (!WmipComputeNameSpace(Block))
        goto Fail;

    return Block;

Fail:
    DPRINT1("WMI: dropping a malformed registration for GUID %08lx\n", RegGuid->Guid.Data1);
    WmipFreeDataBlock(Block);
    return NULL;
}

static
VOID
WmipReleaseProviderMofsLocked(
    _Inout_ PWMIP_PROVIDER Provider)
{
    PWMIP_MOF_REFERENCE Reference;
    PWMIP_MOF_RESOURCE Resource;
    PLIST_ENTRY Entry;

    while (!IsListEmpty(&Provider->MofListHead))
    {
        Entry = RemoveHeadList(&Provider->MofListHead);
        Reference = CONTAINING_RECORD(Entry, WMIP_MOF_REFERENCE, ProviderMofLink);
        Resource = Reference->Resource;
        ExFreePoolWithTag(Reference, TAG_WMI_PROVIDER);

        if (--Resource->RefCount == 0)
        {
            RemoveEntryList(&Resource->MofLink);
            ExFreePoolWithTag(Resource, TAG_WMI_PROVIDER);
        }
    }
}

static
BOOLEAN
WmipMofNamesMatch(
    _In_ PWMIP_MOF_RESOURCE Resource,
    _In_ PCUNICODE_STRING RegistryPath,
    _In_ PCUNICODE_STRING ResourceName)
{
    UNICODE_STRING Path, Name;

    RtlInitUnicodeString(&Path, Resource->RegistryPath);
    RtlInitUnicodeString(&Name, Resource->ResourceName);
    return RtlEqualUnicodeString(&Path, RegistryPath, TRUE) &&
           RtlEqualUnicodeString(&Name, ResourceName, TRUE);
}

/* Records the MOF resource of one registration record, once per provider. */
static
VOID
NTAPI
WmipAddMofResource(
    _Inout_ PWMIP_PROVIDER Provider,
    _In_ PCUNICODE_STRING RegistryPath,
    _In_ PCUNICODE_STRING ResourceName)
{
    PWMIP_MOF_RESOURCE Resource = NULL, Fresh;
    PWMIP_MOF_REFERENCE Reference;
    PLIST_ENTRY Entry;
    SIZE_T Size;

    if (!RegistryPath->Length || !ResourceName->Length)
        return;

    /* Built up front, the lock is a fast mutex and the lists are short */
    Size = sizeof(*Fresh) + RegistryPath->Length + ResourceName->Length + 2 * sizeof(WCHAR);
    Fresh = ExAllocatePoolWithTag(PagedPool, Size, TAG_WMI_PROVIDER);
    Reference = ExAllocatePoolWithTag(PagedPool, sizeof(*Reference), TAG_WMI_PROVIDER);
    if (!Fresh || !Reference)
    {
        if (Fresh)
            ExFreePoolWithTag(Fresh, TAG_WMI_PROVIDER);
        if (Reference)
            ExFreePoolWithTag(Reference, TAG_WMI_PROVIDER);
        return;
    }

    Fresh->RefCount = 1;
    Fresh->RegistryPath = (PWSTR)(Fresh + 1);
    RtlCopyMemory(Fresh->RegistryPath, RegistryPath->Buffer, RegistryPath->Length);
    Fresh->RegistryPath[RegistryPath->Length / sizeof(WCHAR)] = UNICODE_NULL;
    Fresh->ResourceName = Fresh->RegistryPath + RegistryPath->Length / sizeof(WCHAR) + 1;
    RtlCopyMemory(Fresh->ResourceName, ResourceName->Buffer, ResourceName->Length);
    Fresh->ResourceName[ResourceName->Length / sizeof(WCHAR)] = UNICODE_NULL;

    ExAcquireFastMutex(&WmipProviderLock);

    if (Provider->Flags & WMIP_PROVIDER_REMOVED)
        goto Done;

    for (Entry = Provider->MofListHead.Flink; Entry != &Provider->MofListHead; Entry = Entry->Flink)
    {
        if (WmipMofNamesMatch(CONTAINING_RECORD(Entry, WMIP_MOF_REFERENCE, ProviderMofLink)->Resource,
                              RegistryPath,
                              ResourceName))
        {
            goto Done;
        }
    }

    for (Entry = WmipMofListHead.Flink; Entry != &WmipMofListHead; Entry = Entry->Flink)
    {
        if (WmipMofNamesMatch(CONTAINING_RECORD(Entry, WMIP_MOF_RESOURCE, MofLink), RegistryPath, ResourceName))
        {
            Resource = CONTAINING_RECORD(Entry, WMIP_MOF_RESOURCE, MofLink);
            Resource->RefCount++;
            break;
        }
    }

    if (!Resource)
    {
        Resource = Fresh;
        Fresh = NULL;
        InsertTailList(&WmipMofListHead, &Resource->MofLink);
    }

    Reference->Resource = Resource;
    InsertTailList(&Provider->MofListHead, &Reference->ProviderMofLink);
    Reference = NULL;

Done:
    ExReleaseFastMutex(&WmipProviderLock);

    if (Fresh)
        ExFreePoolWithTag(Fresh, TAG_WMI_PROVIDER);
    if (Reference)
        ExFreePoolWithTag(Reference, TAG_WMI_PROVIDER);
}

/**
 * @brief Turns a chain of WMIREGINFO records into unattached data blocks.
 */
static
NTSTATUS
NTAPI
WmipParseRegInfo(
    _Inout_ PWMIP_PROVIDER Provider,
    _In_reads_bytes_(InfoSize) PWMIREGINFOW Info,
    _In_ ULONG InfoSize,
    _In_ BOOLEAN Extended,
    _Inout_ PLIST_ENTRY BlockListHead)
{
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    UNICODE_STRING Path, MofName;
    PWMIP_DATA_BLOCK Block;
    PWMIREGGUIDW RegGuid;
    PDEVICE_OBJECT Pdo;
    ULONG Remaining, Index, Next;

    Remaining = InfoSize;
    for (;;)
    {
        if ((Remaining < (ULONG)FIELD_OFFSET(WMIREGINFOW, WmiRegGuid)) ||
            (Info->BufferSize > Remaining) ||
            (Info->GuidCount > (Remaining - FIELD_OFFSET(WMIREGINFOW, WmiRegGuid)) / sizeof(*RegGuid)) ||
            !WmipGetRegInfoString(Info, Remaining, Info->RegistryPath, &Path) ||
            !WmipGetRegInfoString(Info, Remaining, Info->MofResourceName, &MofName))
        {
            break;
        }

        /* Windows lists the service key and resource name as given, user mode finds the image */
        WmipAddMofResource(Provider, &Path, &MofName);

        for (Index = 0; Index < Info->GuidCount; Index++)
        {
            RegGuid = &Info->WmiRegGuid[Index];
            Pdo = NULL;
            if (RegGuid->Flags & WMIREG_FLAG_INSTANCE_PDO)
                Pdo = (PDEVICE_OBJECT)RegGuid->Pdo;

            Block = WmipCreateDataBlock(Info, Remaining, RegGuid, Pdo);

            /* The extended request hands over a PDO reference for every GUID naming one */
            if (Extended && Pdo)
                ObDereferenceObject(Pdo);

            if (Block)
                InsertTailList(BlockListHead, &Block->BlockLink);
        }

        Status = STATUS_SUCCESS;

        Next = Info->NextWmiRegInfo;
        if ((Next == 0) || (Next > Remaining) || (Next & (sizeof(ULONG) - 1)))
            break;

        Remaining -= Next;
        Info = (PWMIREGINFOW)((PUCHAR)Info + Next);
    }

    return Status;
}

/* Blocks sharing a GUID and base name number their instances one after the other. */
static
ULONG
WmipNextBaseIndexLocked(
    _In_ PWMIP_DATA_BLOCK Block)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    UNICODE_STRING Name, OtherName;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Other;
    ULONG Next = 0;

    RtlInitUnicodeString(&Name, Block->BaseName);
    for (ProviderEntry = WmipProviderListHead.Flink;
         ProviderEntry != &WmipProviderListHead;
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Other = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (!Other->BaseName || !IsEqualGUID(&Other->Guid, &Block->Guid))
                continue;

            RtlInitUnicodeString(&OtherName, Other->BaseName);
            if (RtlEqualUnicodeString(&Name, &OtherName, TRUE) &&
                (Other->BaseIndex + Other->InstanceCount > Next))
            {
                Next = Other->BaseIndex + Other->InstanceCount;
            }
        }
    }

    return Next;
}

/* A first registration replaces everything, an update merges GUID by GUID. */
static
VOID
WmipApplyBlocksLocked(
    _Inout_ PWMIP_PROVIDER Provider,
    _Inout_ PLIST_ENTRY NewBlocks,
    _In_ BOOLEAN Update,
    _Inout_ PLIST_ENTRY Discard)
{
    PWMIP_DATA_BLOCK Block, Existing;
    PLIST_ENTRY Entry, NextEntry;

    if (Provider->Flags & WMIP_PROVIDER_REMOVED)
    {
        WmipMoveBlockListLocked(NewBlocks, Discard);
        return;
    }

    if (!Update)
        WmipMoveBlockListLocked(&Provider->BlockListHead, Discard);

    while (!IsListEmpty(NewBlocks))
    {
        Entry = RemoveHeadList(NewBlocks);
        Block = CONTAINING_RECORD(Entry, WMIP_DATA_BLOCK, BlockLink);

        for (Entry = Provider->BlockListHead.Flink;
             Entry != &Provider->BlockListHead;
             Entry = NextEntry)
        {
            NextEntry = Entry->Flink;
            Existing = CONTAINING_RECORD(Entry, WMIP_DATA_BLOCK, BlockLink);
            if (IsEqualGUID(&Existing->Guid, &Block->Guid))
            {
                /* The replacement inherits whatever the provider was switched to */
                Block->Control |= Existing->Control;
                RemoveEntryList(Entry);
                InsertTailList(Discard, Entry);
            }
        }

        if (Block->RegFlags & WMIREG_FLAG_REMOVE_GUID)
        {
            InsertTailList(Discard, &Block->BlockLink);
            continue;
        }

        if (Block->BaseName)
        {
            Block->BaseIndex = WmipNextBaseIndexLocked(Block);
            if (Block->BaseIndex + Block->InstanceCount > WMIP_STATIC_INDEX_LIMIT)
                Block->BaseIndex = 0;
        }

        Block->Provider = Provider;
        InterlockedIncrement(&Provider->RefCount);
        InsertTailList(&Provider->BlockListHead, &Block->BlockLink);
    }
}

static
VOID
WmipQueueRegInfoLocked(
    _Inout_ PWMIP_PROVIDER Provider)
{
    /* A running worker picks the request up before it exits */
    if (Provider->WorkQueued)
    {
        Provider->WorkPending = TRUE;
        return;
    }

    Provider->WorkQueued = TRUE;
    InterlockedIncrement(&Provider->RefCount);
    ExInitializeWorkItem(&Provider->WorkItem, WmipRegInfoWorker, Provider);
    ExQueueWorkItem(&Provider->WorkItem, DelayedWorkQueue);
}

static
VOID
NTAPI
WmipRegInfoWorker(
    _In_ PVOID Context)
{
    PWMIP_PROVIDER Provider = Context;
    LIST_ENTRY NewBlocks, Discard;
    BOOLEAN Extended, Update, Again;
    PWMIREGINFOW Info;
    NTSTATUS Status;
    ULONG InfoSize;

    PAGED_CODE();

    do
    {
        InitializeListHead(&NewBlocks);
        InitializeListHead(&Discard);

        ExAcquireFastMutex(&WmipProviderLock);
        Update = Provider->Registered;
        ExReleaseFastMutex(&WmipProviderLock);

        Status = WmipQueryRegInfo(Provider,
                                  Update ? WMIUPDATE : WMIREGISTER,
                                  &Info,
                                  &InfoSize,
                                  &Extended);
        if (NT_SUCCESS(Status))
        {
            Status = WmipParseRegInfo(Provider, Info, InfoSize, Extended, &NewBlocks);
            ExFreePoolWithTag(Info, TAG_WMI_PROVIDER);
        }

        if (!NT_SUCCESS(Status))
            DPRINT("WMI: no registration from %p: 0x%lx\n", Provider->DeviceObject, Status);

        ExAcquireFastMutex(&WmipProviderLock);
        if (NT_SUCCESS(Status))
        {
            WmipApplyBlocksLocked(Provider, &NewBlocks, Update, &Discard);
            Provider->Registered = TRUE;
        }

        Again = Provider->WorkPending && !(Provider->Flags & WMIP_PROVIDER_REMOVED);
        Provider->WorkPending = FALSE;
        if (!Again)
            Provider->WorkQueued = FALSE;
        ExReleaseFastMutex(&WmipProviderLock);

        WmipReleaseBlockList(&NewBlocks);
        WmipReleaseBlockList(&Discard);

        /* Consumers that came first get their requests now */
        if (NT_SUCCESS(Status))
            WmipControlNewBlocks(Provider);
    } while (Again);

    WmipDereferenceProvider(Provider);
}

NTSTATUS
NTAPI
WmipRegisterProvider(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PWMIP_PROVIDER Provider;
    KIRQL OldIrql;

    Provider = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Provider), TAG_WMI_PROVIDER);
    if (!Provider)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Provider, sizeof(*Provider));
    InitializeListHead(&Provider->BlockListHead);
    InitializeListHead(&Provider->MofListHead);
    KeInitializeEvent(&Provider->IrpsDrained, NotificationEvent, FALSE);
    Provider->DeviceObject = DeviceObject;
    Provider->RefCount = 1;

    ExAcquireFastMutex(&WmipProviderLock);
    if (WmipFindProviderLocked(DeviceObject))
    {
        ExReleaseFastMutex(&WmipProviderLock);
        ExFreePoolWithTag(Provider, TAG_WMI_PROVIDER);
        return STATUS_OBJECT_NAME_EXISTS;
    }

    /* The registration information is fetched from a worker, as Windows does */
    ObReferenceObject(DeviceObject);
    KeAcquireSpinLock(&WmipProviderIdLock, &OldIrql);
#ifdef _WIN64
    /* Zero means not registered, so it is never handed out */
    if (++WmipLastProviderId == 0)
        ++WmipLastProviderId;
    Provider->ProviderId = WmipLastProviderId;
#else
    Provider->ProviderId = IoWMIDeviceObjectToProviderId(DeviceObject);
#endif
    InsertTailList(&WmipProviderListHead, &Provider->ProviderLink);
    KeReleaseSpinLock(&WmipProviderIdLock, OldIrql);
    WmipQueueRegInfoLocked(Provider);
    ExReleaseFastMutex(&WmipProviderLock);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
WmipDeregisterProvider(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PWMIP_PROVIDER Provider;
    LIST_ENTRY Discard;
    BOOLEAN Wait;
    KIRQL OldIrql;

    InitializeListHead(&Discard);

    ExAcquireFastMutex(&WmipProviderLock);
    Provider = WmipFindProviderLocked(DeviceObject);
    if (!Provider)
    {
        ExReleaseFastMutex(&WmipProviderLock);
        return STATUS_INVALID_PARAMETER;
    }

    Provider->Flags |= WMIP_PROVIDER_REMOVED;
    KeAcquireSpinLock(&WmipProviderIdLock, &OldIrql);
    RemoveEntryList(&Provider->ProviderLink);
    KeReleaseSpinLock(&WmipProviderIdLock, OldIrql);
    WmipMoveBlockListLocked(&Provider->BlockListHead, &Discard);
    WmipReleaseProviderMofsLocked(Provider);
    Wait = (Provider->IrpCount != 0);
    ExReleaseFastMutex(&WmipProviderLock);

    /* The driver may free its WMI context as soon as this returns */
    if (Wait)
        KeWaitForSingleObject(&Provider->IrpsDrained, Executive, KernelMode, FALSE, NULL);

    WmipReleaseBlockList(&Discard);
    WmipDereferenceProvider(Provider);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
WmipUpdateProvider(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PWMIP_PROVIDER Provider;

    ExAcquireFastMutex(&WmipProviderLock);
    Provider = WmipFindProviderLocked(DeviceObject);
    if (Provider)
        WmipQueueRegInfoLocked(Provider);
    ExReleaseFastMutex(&WmipProviderLock);

    return Provider ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS
NTAPI
WmipBlockProvider(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PWMIP_PROVIDER Provider;

    ExAcquireFastMutex(&WmipProviderLock);
    Provider = WmipFindProviderLocked(DeviceObject);
    if (Provider)
        Provider->Flags |= WMIP_PROVIDER_BLOCKED;
    ExReleaseFastMutex(&WmipProviderLock);

    return Provider ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static
BOOLEAN
WmipIsDataBlockFor(
    _In_ PWMIP_DATA_BLOCK Block,
    _In_ LPCGUID Guid)
{
    return IsEqualGUID(&Block->Guid, Guid) && !(Block->RegFlags & WMIP_NON_DATA_FLAGS);
}

BOOLEAN
NTAPI
WmipIsQuerySetGuid(
    _In_ LPCGUID Guid)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;
    BOOLEAN Found = FALSE;

    ExAcquireFastMutex(&WmipProviderLock);
    for (ProviderEntry = WmipProviderListHead.Flink;
         !Found && (ProviderEntry != &WmipProviderListHead);
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (WmipIsDataBlockFor(Block, Guid))
            {
                Found = TRUE;
                break;
            }
        }
    }
    ExReleaseFastMutex(&WmipProviderLock);

    return Found;
}

/**
 * @brief Returns a referenced snapshot of every data block registered for Guid.
 */
static
NTSTATUS
NTAPI
WmipCollectDataBlocks(
    _In_ LPCGUID Guid,
    _Outptr_result_buffer_(*Count) PWMIP_DATA_BLOCK **OutBlocks,
    _Out_ PULONG Count)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;
    PWMIP_DATA_BLOCK *Blocks;
    ULONG Found = 0, Filled = 0;
    BOOLEAN Collect;

    *OutBlocks = NULL;
    *Count = 0;

    ExAcquireFastMutex(&WmipProviderLock);

    /* The first pass counts, the second one fills */
    Blocks = NULL;
    for (Collect = FALSE; ; Collect = TRUE)
    {
        for (ProviderEntry = WmipProviderListHead.Flink;
             ProviderEntry != &WmipProviderListHead;
             ProviderEntry = ProviderEntry->Flink)
        {
            Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
            for (BlockEntry = Provider->BlockListHead.Flink;
                 BlockEntry != &Provider->BlockListHead;
                 BlockEntry = BlockEntry->Flink)
            {
                Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
                if (!WmipIsDataBlockFor(Block, Guid))
                    continue;

                if (!Collect)
                {
                    Found++;
                    continue;
                }

                InterlockedIncrement(&Block->RefCount);
                Blocks[Filled++] = Block;
            }
        }

        if (Collect)
            break;

        if (Found == 0)
        {
            ExReleaseFastMutex(&WmipProviderLock);
            return STATUS_WMI_GUID_DISCONNECTED;
        }

        Blocks = ExAllocatePoolWithTag(PagedPool, Found * sizeof(*Blocks), TAG_WMI_PROVIDER);
        if (!Blocks)
        {
            ExReleaseFastMutex(&WmipProviderLock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    ExReleaseFastMutex(&WmipProviderLock);

    *OutBlocks = Blocks;
    *Count = Filled;
    return STATUS_SUCCESS;
}

static
VOID
WmipReleaseDataBlocks(
    _In_reads_(Count) PWMIP_DATA_BLOCK *Blocks,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        WmipDereferenceDataBlock(Blocks[Index]);
    ExFreePoolWithTag(Blocks, TAG_WMI_PROVIDER);
}

static
BOOLEAN
WmipHasStaticNames(
    _In_ PWMIP_DATA_BLOCK Block)
{
    return (Block->BaseName != NULL) || (Block->StaticNames != NULL);
}

/**
 * @brief Adds the instance names WMI owns for a block behind the data its provider returned.
 */
static
VOID
NTAPI
WmipAppendStaticNames(
    _Inout_updates_bytes_(MaxSize) PWNODE_ALL_DATA Wnode,
    _In_ ULONG MaxSize,
    _In_ PWMIP_DATA_BLOCK Block)
{
    WCHAR IndexBuffer[12];
    UNICODE_STRING IndexString;
    SIZE_T NameLength, IndexLength;
    PUCHAR Cursor, End;
    PULONG NameOffsets;
    ULONG Start, Index;
    PCWSTR Name;

    Start = (ULONG)WMIP_ALIGN(Wnode->WnodeHeader.BufferSize, sizeof(ULONG));
    if ((Start < Wnode->WnodeHeader.BufferSize) || (Block->NameSpace > MAXULONG - Start))
        return;

    if (Start + Block->NameSpace > MaxSize)
    {
        Wnode->WnodeHeader.BufferSize = sizeof(WNODE_TOO_SMALL);
        Wnode->WnodeHeader.Flags = WNODE_FLAG_TOO_SMALL;
        ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded = Start + Block->NameSpace;
        return;
    }

    Wnode->OffsetInstanceNameOffsets = Start;
    NameOffsets = (PULONG)((PUCHAR)Wnode + Start);
    Cursor = (PUCHAR)(NameOffsets + Block->InstanceCount);
    End = (PUCHAR)Wnode + Start + Block->NameSpace;

    if (Block->BaseName && (Block->RegFlags & WMIREG_FLAG_INSTANCE_PDO))
        Wnode->WnodeHeader.Flags |= WNODE_FLAG_PDO_INSTANCE_NAMES;

    for (Index = 0; Index < Block->InstanceCount; Index++)
    {
        IndexLength = 0;
        if (Block->BaseName)
        {
            Name = Block->BaseName;
            RtlInitEmptyUnicodeString(&IndexString, IndexBuffer, sizeof(IndexBuffer));
            RtlIntegerToUnicodeString(Block->BaseIndex + Index, 10, &IndexString);
            IndexLength = IndexString.Length;
        }
        else
        {
            Name = Block->StaticNames[Index];
        }

        NameLength = wcslen(Name) * sizeof(WCHAR);
        if (sizeof(USHORT) + NameLength + IndexLength > (SIZE_T)(End - Cursor))
            break;

        /* Each name is a byte count followed by the characters, no terminator */
        NameOffsets[Index] = (ULONG)(Cursor - (PUCHAR)Wnode);
        *(PUSHORT)Cursor = (USHORT)(NameLength + IndexLength);
        Cursor += sizeof(USHORT);
        RtlCopyMemory(Cursor, Name, NameLength);
        Cursor += NameLength;
        RtlCopyMemory(Cursor, IndexBuffer, IndexLength);
        Cursor += IndexLength;
    }

    for (; Index < Block->InstanceCount; Index++)
        NameOffsets[Index] = 0;

    Wnode->WnodeHeader.BufferSize = Start + Block->NameSpace;
}

/**
 * @brief Collects WNODE_ALL_DATA from every provider of a GUID, chained through WNODE_HEADER.Linkage.
 *
 * When the buffer runs out the whole result becomes a WNODE_TOO_SMALL with the total size needed.
 */
NTSTATUS
NTAPI
WmipQueryAllData(
    _In_ PWMIP_GUID_OBJECT GuidObject,
    _Inout_updates_bytes_(BufferSize) PWNODE_ALL_DATA Wnode,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize)
{
    PWNODE_HEADER Previous = NULL;
    PWMIP_DATA_BLOCK *Blocks;
    PWMIP_DATA_BLOCK Block;
    PWNODE_ALL_DATA Current;
    ULONG64 Probe[WMIP_MIN_ALL_DATA_SIZE / sizeof(ULONG64)];
    ULONG_PTR Information;
    ULONGLONG Needed = 0;
    ULONGLONG Size;
    ULONG Count, Index, Flags, Left;
    BOOLEAN TooSmall = FALSE;
    BOOLEAN Probing = FALSE;
    BOOLEAN StaticNames;
    PUCHAR Cursor;
    NTSTATUS Status;

    PAGED_CODE();

    *ReturnSize = 0;

    Status = WmipCollectDataBlocks(&GuidObject->Guid, &Blocks, &Count);
    if (!NT_SUCCESS(Status))
        return Status;

    Flags = Wnode->WnodeHeader.Flags;
    Cursor = (PUCHAR)Wnode;
    Left = BufferSize;

    for (Index = 0; Index < Count; Index++)
    {
        Block = Blocks[Index];
        StaticNames = WmipHasStaticNames(Block);

        /* Out of room, the remaining providers only tell how much they need */
        if (TooSmall || Probing || (Left < WMIP_MIN_ALL_DATA_SIZE))
        {
            RtlZeroMemory(Probe, sizeof(Probe));
            Current = (PWNODE_ALL_DATA)Probe;
            Left = sizeof(Probe);
            Probing = TRUE;
        }
        else
        {
            Current = (PWNODE_ALL_DATA)Cursor;
        }

        Current->WnodeHeader.BufferSize = sizeof(WNODE_HEADER);
        Current->WnodeHeader.Flags = Flags | (StaticNames ? WNODE_FLAG_STATIC_INSTANCE_NAMES : 0);
        Current->WnodeHeader.Guid = GuidObject->Guid;
        Current->WnodeHeader.ProviderId = Block->Provider->ProviderId;
        Current->WnodeHeader.Linkage = 0;

        Status = WmipSendIrp(Block->Provider,
                             IRP_MN_QUERY_ALL_DATA,
                             &Current->WnodeHeader.Guid,
                             Left,
                             Current,
                             &Information);
        if (!NT_SUCCESS(Status))
            continue;

        if (!Probing && !(Current->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL))
        {
            /* The data stays where it landed, linked behind the previous provider's */
            if (Previous)
                Previous->Linkage = (ULONG)((PUCHAR)Current - (PUCHAR)Previous);
            Previous = &Current->WnodeHeader;

            if (StaticNames)
                WmipAppendStaticNames(Current, Left, Block);

            if (Current->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
            {
                Size = ((PWNODE_TOO_SMALL)Current)->SizeNeeded;
                TooSmall = TRUE;
            }
            else
            {
                Size = Current->WnodeHeader.BufferSize;
            }

            Size = WMIP_ALIGN(Size, 8);
            Needed += Size;
            if (TooSmall || (Left < Size))
            {
                TooSmall = TRUE;
            }
            else
            {
                Left -= (ULONG)Size;
                Cursor += (ULONG)Size;
            }
            continue;
        }

        if (Current->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL)
        {
            Size = ((PWNODE_TOO_SMALL)Current)->SizeNeeded;

            /* Asking for no more than it was given is a broken answer */
            if (Size <= Left)
                continue;
        }
        else
        {
            Size = Current->WnodeHeader.BufferSize;
        }

        if (Size < WMIP_MIN_ALL_DATA_SIZE)
            Size = WMIP_MIN_ALL_DATA_SIZE;

        Needed += Size;
        if (StaticNames)
            Needed = WMIP_ALIGN(Needed, sizeof(ULONG)) + Block->NameSpace;
        Needed = WMIP_ALIGN(Needed, 8);
        TooSmall = TRUE;
    }

    WmipReleaseDataBlocks(Blocks, Count);

    if (Needed == 0)
        return STATUS_WMI_GUID_NOT_FOUND;

    if (!TooSmall)
    {
        *ReturnSize = (ULONG)Needed;
        return STATUS_SUCCESS;
    }

    /* Never report a size the caller already offered */
    if (Needed <= BufferSize)
        Needed = (ULONGLONG)BufferSize + 64;
    if (Needed > MAXULONG)
        Needed = MAXULONG;

    Wnode->WnodeHeader.BufferSize = sizeof(WNODE_TOO_SMALL);
    Wnode->WnodeHeader.Flags = WNODE_FLAG_TOO_SMALL;
    ((PWNODE_TOO_SMALL)Wnode)->SizeNeeded = (ULONG)Needed;
    *ReturnSize = sizeof(WNODE_TOO_SMALL);
    return STATUS_SUCCESS;
}

/* Base names match as the base followed by a decimal index without leading zeros. */
static
BOOLEAN
NTAPI
WmipMatchStaticName(
    _In_ PWMIP_DATA_BLOCK Block,
    _In_ PCUNICODE_STRING Name,
    _Out_ PULONG InstanceIndex)
{
    UNICODE_STRING Candidate;
    ULONG Index, Value, Chars;
    PCWCH Suffix;

    *InstanceIndex = 0;

    if (Block->StaticNames)
    {
        for (Index = 0; Index < Block->InstanceCount; Index++)
        {
            RtlInitUnicodeString(&Candidate, Block->StaticNames[Index]);
            if (RtlEqualUnicodeString(&Candidate, Name, TRUE))
            {
                *InstanceIndex = Index;
                return TRUE;
            }
        }
        return FALSE;
    }

    RtlInitUnicodeString(&Candidate, Block->BaseName);
    if ((Name->Length <= Candidate.Length) || !RtlPrefixUnicodeString(&Candidate, Name, TRUE))
        return FALSE;

    Suffix = Name->Buffer + Candidate.Length / sizeof(WCHAR);
    Chars = (Name->Length - Candidate.Length) / sizeof(WCHAR);
    if ((Chars > 6) || ((Chars > 1) && (Suffix[0] == L'0')))
        return FALSE;

    Value = 0;
    for (Index = 0; Index < Chars; Index++)
    {
        if ((Suffix[Index] < L'0') || (Suffix[Index] > L'9'))
            return FALSE;
        Value = Value * 10 + (Suffix[Index] - L'0');
    }

    if ((Value >= WMIP_STATIC_INDEX_LIMIT) ||
        (Value < Block->BaseIndex) ||
        (Value - Block->BaseIndex >= Block->InstanceCount))
    {
        return FALSE;
    }

    *InstanceIndex = Value - Block->BaseIndex;
    return TRUE;
}


C_ASSERT(FIELD_OFFSET(WNODE_SINGLE_ITEM, OffsetInstanceName) == FIELD_OFFSET(WNODE_SINGLE_INSTANCE, OffsetInstanceName));
C_ASSERT(FIELD_OFFSET(WNODE_SINGLE_ITEM, InstanceIndex) == FIELD_OFFSET(WNODE_SINGLE_INSTANCE, InstanceIndex));
C_ASSERT(FIELD_OFFSET(WNODE_METHOD_ITEM, OffsetInstanceName) == FIELD_OFFSET(WNODE_SINGLE_INSTANCE, OffsetInstanceName));
C_ASSERT(FIELD_OFFSET(WNODE_METHOD_ITEM, InstanceIndex) == FIELD_OFFSET(WNODE_SINGLE_INSTANCE, InstanceIndex));

/* What a request ends with when no dynamic name provider claims the instance. */
static
NTSTATUS
WmipUnclaimedInstanceStatus(
    _In_ UCHAR MinorFunction)
{
    if ((MinorFunction == IRP_MN_CHANGE_SINGLE_ITEM) || (MinorFunction == IRP_MN_EXECUTE_METHOD))
        return STATUS_WMI_ITEMID_NOT_FOUND;

    return STATUS_WMI_INSTANCE_NOT_FOUND;
}

/**
 * @brief Routes a request on one named instance to the provider that owns it.
 *
 * Queries, sets and method calls all carry the name and index at the WNODE_SINGLE_INSTANCE offsets.
 * Static names are resolved here, dynamic name providers are asked in turn until one claims it.
 */
NTSTATUS
NTAPI
WmipSendSingleInstanceRequest(
    _In_ PWMIP_GUID_OBJECT GuidObject,
    _In_ UCHAR MinorFunction,
    _Inout_updates_bytes_(BufferSize) PWNODE_SINGLE_INSTANCE Wnode,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize)
{
    PWMIP_DATA_BLOCK Target = NULL;
    PWMIP_DATA_BLOCK *Blocks;
    PWMIP_DATA_BLOCK Block;
    NTSTATUS Status, ProviderStatus;
    UNICODE_STRING Name;
    ULONG_PTR Information;
    ULONG Count, Index;
    ULONG Instance = 0;
    BOOLEAN Dynamic = FALSE;

    PAGED_CODE();

    *ReturnSize = 0;
    Wnode->WnodeHeader.Guid = GuidObject->Guid;

    Status = WmipCollectDataBlocks(&GuidObject->Guid, &Blocks, &Count);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The caller validated the counted name at OffsetInstanceName */
    Name.Length = *(PUSHORT)((PUCHAR)Wnode + Wnode->OffsetInstanceName) & ~(sizeof(WCHAR) - 1);
    Name.MaximumLength = Name.Length;
    Name.Buffer = (PWCH)((PUCHAR)Wnode + Wnode->OffsetInstanceName + sizeof(USHORT));

    for (Index = 0; Index < Count; Index++)
    {
        if (WmipHasStaticNames(Blocks[Index]) &&
            WmipMatchStaticName(Blocks[Index], &Name, &Instance))
        {
            Target = Blocks[Index];
            break;
        }
    }

    if (Target)
    {
        Wnode->WnodeHeader.Flags |= WNODE_FLAG_STATIC_INSTANCE_NAMES;
        Wnode->WnodeHeader.ProviderId = Target->Provider->ProviderId;
        Wnode->InstanceIndex = Instance;

        Status = WmipSendIrp(Target->Provider,
                             MinorFunction,
                             &Wnode->WnodeHeader.Guid,
                             BufferSize,
                             Wnode,
                             &Information);
        if (NT_SUCCESS(Status))
            *ReturnSize = (ULONG)Information;
    }
    else
    {
        Status = STATUS_WMI_INSTANCE_NOT_FOUND;
        for (Index = 0; Index < Count; Index++)
        {
            Block = Blocks[Index];
            if (WmipHasStaticNames(Block))
                continue;

            if (!Dynamic)
            {
                Dynamic = TRUE;
                Status = WmipUnclaimedInstanceStatus(MinorFunction);
            }

            Wnode->WnodeHeader.ProviderId = Block->Provider->ProviderId;
            ProviderStatus = WmipSendIrp(Block->Provider,
                                         MinorFunction,
                                         &Wnode->WnodeHeader.Guid,
                                         BufferSize,
                                         Wnode,
                                         &Information);
            if (NT_SUCCESS(ProviderStatus))
                *ReturnSize = (ULONG)Information;

            if ((ProviderStatus != STATUS_WMI_INSTANCE_NOT_FOUND) &&
                (ProviderStatus != STATUS_WMI_GUID_NOT_FOUND))
            {
                Status = ProviderStatus;
                break;
            }
        }
    }

    WmipReleaseDataBlocks(Blocks, Count);

    if (NT_SUCCESS(Status) && (*ReturnSize > BufferSize))
        Status = STATUS_INVALID_PARAMETER;

    return Status;
}

/* Whether a switch request still has to reach a block, collection only concerns expensive ones. */
static
BOOLEAN
WmipBlockNeedsControl(
    _In_ PWMIP_DATA_BLOCK Block,
    _In_ UCHAR MinorFunction)
{
    if (Block->RegFlags & WMIREG_FLAG_TRACED_GUID)
        return FALSE;

    switch (MinorFunction)
    {
        case IRP_MN_ENABLE_EVENTS:
            return !(Block->Control & WMIP_BLOCK_EVENTS_ON);

        case IRP_MN_DISABLE_EVENTS:
            return (Block->Control & WMIP_BLOCK_EVENTS_ON) != 0;

        case IRP_MN_ENABLE_COLLECTION:
            return (Block->RegFlags & WMIREG_FLAG_EXPENSIVE) && !(Block->Control & WMIP_BLOCK_COLLECTION_ON);

        case IRP_MN_DISABLE_COLLECTION:
            return (Block->Control & WMIP_BLOCK_COLLECTION_ON) != 0;

        default:
            return FALSE;
    }
}

static
VOID
WmipMarkControl(
    _Inout_ PWMIP_DATA_BLOCK Block,
    _In_ UCHAR MinorFunction)
{
    switch (MinorFunction)
    {
        case IRP_MN_ENABLE_EVENTS:
            Block->Control |= WMIP_BLOCK_EVENTS_ON;
            break;

        case IRP_MN_DISABLE_EVENTS:
            Block->Control &= ~WMIP_BLOCK_EVENTS_ON;
            break;

        case IRP_MN_ENABLE_COLLECTION:
            Block->Control |= WMIP_BLOCK_COLLECTION_ON;
            break;

        case IRP_MN_DISABLE_COLLECTION:
            Block->Control &= ~WMIP_BLOCK_COLLECTION_ON;
            break;
    }
}

/* The switch requests carry a bare WNODE_HEADER naming the GUID, as Windows sends them. */
static
VOID
WmipSendControlIrp(
    _In_ PWMIP_DATA_BLOCK Block,
    _In_ UCHAR MinorFunction)
{
    ULONG_PTR Information;
    WNODE_HEADER Header;
    NTSTATUS Status;

    RtlZeroMemory(&Header, sizeof(Header));
    Header.BufferSize = sizeof(Header);
    Header.Guid = Block->Guid;

    Status = WmipSendIrp(Block->Provider,
                         MinorFunction,
                         &Header.Guid,
                         sizeof(Header),
                         &Header,
                         &Information);
    if (!NT_SUCCESS(Status))
        DPRINT("WMI: switch request %u for %08lx failed: 0x%lx\n", MinorFunction, Block->Guid.Data1, Status);
}

/**
 * @brief Sends an enable or disable request to every block of a GUID not already in that state.
 *
 * The caller holds the consumer lock, which keeps opposite requests for one GUID in order.
 */
VOID
NTAPI
WmipControlGuidLocked(
    _In_ LPCGUID Guid,
    _In_ UCHAR MinorFunction)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMIP_DATA_BLOCK *Blocks = NULL;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;
    ULONG Count = 0, Filled = 0, Index;
    BOOLEAN Collect;

    PAGED_CODE();

    ExAcquireFastMutex(&WmipProviderLock);

    for (Collect = FALSE; ; Collect = TRUE)
    {
        for (ProviderEntry = WmipProviderListHead.Flink;
             ProviderEntry != &WmipProviderListHead;
             ProviderEntry = ProviderEntry->Flink)
        {
            Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
            for (BlockEntry = Provider->BlockListHead.Flink;
                 BlockEntry != &Provider->BlockListHead;
                 BlockEntry = BlockEntry->Flink)
            {
                Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
                if (!IsEqualGUID(&Block->Guid, Guid) || !WmipBlockNeedsControl(Block, MinorFunction))
                    continue;

                if (!Collect)
                {
                    Count++;
                    continue;
                }

                WmipMarkControl(Block, MinorFunction);
                InterlockedIncrement(&Block->RefCount);
                Blocks[Filled++] = Block;
            }
        }

        if (Collect || (Count == 0))
            break;

        Blocks = ExAllocatePoolWithTag(PagedPool, Count * sizeof(*Blocks), TAG_WMI_PROVIDER);
        if (!Blocks)
            break;
    }

    ExReleaseFastMutex(&WmipProviderLock);

    for (Index = 0; Index < Filled; Index++)
    {
        WmipSendControlIrp(Blocks[Index], MinorFunction);
        WmipDereferenceDataBlock(Blocks[Index]);
    }

    if (Blocks)
        ExFreePoolWithTag(Blocks, TAG_WMI_PROVIDER);
}

/* Blocks registered after their consumers opened them are switched on here. */
static
VOID
NTAPI
WmipControlNewBlocks(
    _Inout_ PWMIP_PROVIDER Provider)
{
    static const UCHAR Requests[] = { IRP_MN_ENABLE_EVENTS, IRP_MN_ENABLE_COLLECTION };
    PWMIP_DATA_BLOCK Target, Block;
    PLIST_ENTRY Entry;
    UCHAR MinorFunction = 0;
    ULONG OpenType, Index;

    PAGED_CODE();

    /* Nobody listens during boot, which keeps the registration path free of waits */
    if (!WmipHasAnyConsumer())
        return;

    WmipAcquireConsumerLock();

    for (;;)
    {
        Target = NULL;

        ExAcquireFastMutex(&WmipProviderLock);
        if (!(Provider->Flags & WMIP_PROVIDER_REMOVED))
        {
            for (Entry = Provider->BlockListHead.Flink;
                 !Target && (Entry != &Provider->BlockListHead);
                 Entry = Entry->Flink)
            {
                Block = CONTAINING_RECORD(Entry, WMIP_DATA_BLOCK, BlockLink);
                for (Index = 0; Index < RTL_NUMBER_OF(Requests); Index++)
                {
                    OpenType = (Requests[Index] == IRP_MN_ENABLE_EVENTS) ? WMIP_OPEN_EVENTS : WMIP_OPEN_QUERYSET;
                    if (WmipBlockNeedsControl(Block, Requests[Index]) &&
                        WmipHasConsumerLocked(&Block->Guid, OpenType))
                    {
                        MinorFunction = Requests[Index];
                        WmipMarkControl(Block, MinorFunction);
                        InterlockedIncrement(&Block->RefCount);
                        Target = Block;
                        break;
                    }
                }
            }
        }
        ExReleaseFastMutex(&WmipProviderLock);

        if (!Target)
            break;

        WmipSendControlIrp(Target, MinorFunction);
        WmipDereferenceDataBlock(Target);
    }

    WmipReleaseConsumerLock();
}

NTSTATUS
NTAPI
WmipQueryGuidExpense(
    _In_ LPCGUID Guid,
    _Out_ PBOOLEAN IsExpensive)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;

    *IsExpensive = FALSE;

    ExAcquireFastMutex(&WmipProviderLock);
    for (ProviderEntry = WmipProviderListHead.Flink;
         !*IsExpensive && (ProviderEntry != &WmipProviderListHead);
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (IsEqualGUID(&Block->Guid, Guid) && (Block->RegFlags & WMIREG_FLAG_EXPENSIVE))
            {
                *IsExpensive = TRUE;
                break;
            }
        }
    }
    ExReleaseFastMutex(&WmipProviderLock);

    return STATUS_SUCCESS;
}

/* True for the first block of its GUID in list order, so every GUID is listed once. */
static
BOOLEAN
WmipIsFirstOfGuidLocked(
    _In_ PWMIP_DATA_BLOCK Target)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;

    for (ProviderEntry = WmipProviderListHead.Flink;
         ProviderEntry != &WmipProviderListHead;
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (Block == Target)
                return TRUE;
            if (IsEqualGUID(&Block->Guid, &Target->Guid))
                return FALSE;
        }
    }

    return TRUE;
}

static
VOID
WmipFillGuidPropertiesLocked(
    _Inout_ PWMI_GUID_PROPERTIES Properties,
    _In_ LPCGUID Guid)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;

    Properties->GuidType = WMI_GUIDTYPE_DATA;

    for (ProviderEntry = WmipProviderListHead.Flink;
         ProviderEntry != &WmipProviderListHead;
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (!IsEqualGUID(&Block->Guid, Guid))
                continue;

            if (Block->Control)
                Properties->IsEnabled = TRUE;

            if ((Block->RegFlags & WMIREG_FLAG_TRACED_GUID) &&
                (Block->RegFlags & WMIREG_FLAG_TRACE_CONTROL_GUID))
            {
                Properties->GuidType = WMI_GUIDTYPE_TRACE;
                return;
            }

            if (Block->RegFlags & WMIREG_FLAG_EVENT_ONLY_GUID)
                Properties->GuidType = WMI_GUIDTYPE_EVENT;
        }
    }
}

/**
 * @brief Lists every registered GUID once, as many as fit, along with the total count.
 */
NTSTATUS
NTAPI
WmipEnumerateGuids(
    _Out_writes_bytes_(BufferSize) PWMI_GUID_LIST GuidList,
    _In_ ULONG BufferSize,
    _In_ BOOLEAN WithProperties,
    _Out_ PULONG ReturnSize)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMI_GUID_PROPERTIES Properties;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;
    ULONG Total = 0, Returned = 0, Room;

    Room = (BufferSize - FIELD_OFFSET(WMI_GUID_LIST, Guids)) / sizeof(*Properties);

    ExAcquireFastMutex(&WmipProviderLock);
    for (ProviderEntry = WmipProviderListHead.Flink;
         ProviderEntry != &WmipProviderListHead;
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (!WmipIsFirstOfGuidLocked(Block))
                continue;

            Total++;
            if (Returned >= Room)
                continue;

            Properties = &GuidList->Guids[Returned++];
            RtlZeroMemory(Properties, sizeof(*Properties));
            Properties->Guid = Block->Guid;
            if (WithProperties)
                WmipFillGuidPropertiesLocked(Properties, &Block->Guid);
        }
    }
    ExReleaseFastMutex(&WmipProviderLock);

    GuidList->TotalGuidCount = Total;
    GuidList->ReturnedGuidCount = Returned;
    *ReturnSize = FIELD_OFFSET(WMI_GUID_LIST, Guids) + Returned * sizeof(*Properties);
    return STATUS_SUCCESS;
}

/**
 * @brief Lists the registry path and resource name of every MOF resource providers reported.
 *
 * A buffer too small for the list gets the size needed in Count and a return size of one ULONG.
 */
NTSTATUS
NTAPI
WmipEnumerateMofResources(
    _Out_writes_bytes_(BufferSize) PWMI_MOF_LIST MofList,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize)
{
    PWMIP_MOF_RESOURCE Resource;
    PWMI_MOF_ENTRY MofEntry;
    SIZE_T PathSize, NameSize;
    ULONGLONG Needed;
    PLIST_ENTRY Entry;
    ULONG Count = 0, Offset;

    ExAcquireFastMutex(&WmipProviderLock);

    Needed = FIELD_OFFSET(WMI_MOF_LIST, Entries);
    for (Entry = WmipMofListHead.Flink; Entry != &WmipMofListHead; Entry = Entry->Flink)
    {
        Resource = CONTAINING_RECORD(Entry, WMIP_MOF_RESOURCE, MofLink);
        Count++;
        Needed += sizeof(*MofEntry) +
                  (wcslen(Resource->RegistryPath) + wcslen(Resource->ResourceName) + 2) * sizeof(WCHAR);
    }

    if (Count == 0)
    {
        MofList->Count = 0;
        *ReturnSize = sizeof(*MofList);
    }
    else if (Needed > BufferSize)
    {
        MofList->Count = (Needed > MAXULONG) ? MAXULONG : (ULONG)Needed;
        *ReturnSize = sizeof(ULONG);
    }
    else
    {
        MofList->Count = Count;
        MofEntry = MofList->Entries;
        Offset = FIELD_OFFSET(WMI_MOF_LIST, Entries) + Count * sizeof(*MofEntry);

        for (Entry = WmipMofListHead.Flink; Entry != &WmipMofListHead; Entry = Entry->Flink)
        {
            Resource = CONTAINING_RECORD(Entry, WMIP_MOF_RESOURCE, MofLink);
            PathSize = (wcslen(Resource->RegistryPath) + 1) * sizeof(WCHAR);
            NameSize = (wcslen(Resource->ResourceName) + 1) * sizeof(WCHAR);

            MofEntry->Flags = 0;
            MofEntry->RegistryPathOffset = Offset;
            RtlCopyMemory((PUCHAR)MofList + Offset, Resource->RegistryPath, PathSize);
            Offset += (ULONG)PathSize;

            MofEntry->ResourceNameOffset = Offset;
            RtlCopyMemory((PUCHAR)MofList + Offset, Resource->ResourceName, NameSize);
            Offset += (ULONG)NameSize;
            MofEntry++;
        }

        *ReturnSize = Offset;
    }

    ExReleaseFastMutex(&WmipProviderLock);
    return STATUS_SUCCESS;
}

/**
 * @brief Finds the base index of the PDO named instances a GUID has for one device.
 *
 * InstancePath points into the device node of Pdo, which the caller keeps referenced.
 */
NTSTATUS
NTAPI
WmipGetPdoInstanceInfo(
    _In_ LPCGUID Guid,
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PUNICODE_STRING InstancePath,
    _Out_ PULONG BaseIndex)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    NTSTATUS Status = STATUS_WMI_INSTANCE_NOT_FOUND;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;
    PDEVICE_NODE DeviceNode;
    UNICODE_STRING Base;

    RtlZeroMemory(InstancePath, sizeof(*InstancePath));
    *BaseIndex = 0;

    DeviceNode = IopGetDeviceNode(Pdo);
    if (!DeviceNode || !DeviceNode->InstancePath.Length)
        return STATUS_INVALID_DEVICE_REQUEST;

    ExAcquireFastMutex(&WmipProviderLock);
    for (ProviderEntry = WmipProviderListHead.Flink;
         !NT_SUCCESS(Status) && (ProviderEntry != &WmipProviderListHead);
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (!Block->BaseName || !IsEqualGUID(&Block->Guid, Guid))
                continue;

            /* PDO base names are the instance path followed by an underscore */
            RtlInitUnicodeString(&Base, Block->BaseName);
            if ((Base.Length != DeviceNode->InstancePath.Length + sizeof(WCHAR)) ||
                (Base.Buffer[Base.Length / sizeof(WCHAR) - 1] != L'_'))
            {
                continue;
            }

            Base.Length -= sizeof(WCHAR);
            if (RtlEqualUnicodeString(&Base, &DeviceNode->InstancePath, TRUE))
            {
                *BaseIndex = Block->BaseIndex;
                Status = STATUS_SUCCESS;
                break;
            }
        }
    }
    ExReleaseFastMutex(&WmipProviderLock);

    if (NT_SUCCESS(Status))
        *InstancePath = DeviceNode->InstancePath;

    return Status;
}

/* Bytes the counted static name of one instance takes at most. */
static
SIZE_T
WmipStaticNameSize(
    _In_ PWMIP_DATA_BLOCK Block,
    _In_ ULONG Index)
{
    if (Block->StaticNames)
        return sizeof(USHORT) + wcslen(Block->StaticNames[Index]) * sizeof(WCHAR);

    /* Base, then a decimal index of up to seven digits */
    return sizeof(USHORT) + (wcslen(Block->BaseName) + 7) * sizeof(WCHAR);
}

/* Writes the counted static name of one instance into room sized by WmipStaticNameSize. */
static
ULONG
WmipPutStaticName(
    _In_ PWMIP_DATA_BLOCK Block,
    _In_ ULONG Index,
    _Out_ PUSHORT Counted)
{
    WCHAR IndexBuffer[12];
    UNICODE_STRING IndexString;
    SIZE_T NameLength;
    PCWSTR Name;

    Name = Block->StaticNames ? Block->StaticNames[Index] : Block->BaseName;
    NameLength = wcslen(Name) * sizeof(WCHAR);
    RtlCopyMemory(Counted + 1, Name, NameLength);

    if (!Block->StaticNames)
    {
        RtlInitEmptyUnicodeString(&IndexString, IndexBuffer, sizeof(IndexBuffer));
        RtlIntegerToUnicodeString(Block->BaseIndex + Index, 10, &IndexString);
        RtlCopyMemory((PUCHAR)(Counted + 1) + NameLength, IndexBuffer, IndexString.Length);
        NameLength += IndexString.Length;
    }

    *Counted = (USHORT)NameLength;
    return (ULONG)(sizeof(USHORT) + NameLength);
}

static
PWMIP_DATA_BLOCK
WmipFindEventBlockLocked(
    _In_ LPCGUID Guid,
    _In_ ULONG ProviderId)
{
    PLIST_ENTRY ProviderEntry, BlockEntry;
    PWMIP_PROVIDER Provider;
    PWMIP_DATA_BLOCK Block;

    for (ProviderEntry = WmipProviderListHead.Flink;
         ProviderEntry != &WmipProviderListHead;
         ProviderEntry = ProviderEntry->Flink)
    {
        Provider = CONTAINING_RECORD(ProviderEntry, WMIP_PROVIDER, ProviderLink);
        if (Provider->ProviderId != ProviderId)
            continue;

        for (BlockEntry = Provider->BlockListHead.Flink;
             BlockEntry != &Provider->BlockListHead;
             BlockEntry = BlockEntry->Flink)
        {
            Block = CONTAINING_RECORD(BlockEntry, WMIP_DATA_BLOCK, BlockLink);
            if (IsEqualGUID(&Block->Guid, Guid))
                return Block;
        }
    }

    return NULL;
}

/**
 * @brief Gives an event fired by instance index the static names its provider registered.
 *
 * Returns a copy with the names behind the data, or the event itself when no names apply.
 */
static
PWNODE_HEADER
NTAPI
WmipAddEventNames(
    _Inout_ PWNODE_HEADER Wnode)
{
    PWNODE_SINGLE_INSTANCE Single;
    PWMIP_DATA_BLOCK Block;
    PWNODE_HEADER Copy = NULL;
    ULONGLONG Start = 0, Size;
    ULONG Index = 0;

    if (!(Wnode->Flags & (WNODE_FLAG_ALL_DATA | WNODE_FLAG_SINGLE_INSTANCE | WNODE_FLAG_SINGLE_ITEM)))
        return Wnode;

    ExAcquireFastMutex(&WmipProviderLock);

    Block = WmipFindEventBlockLocked(&Wnode->Guid, Wnode->ProviderId);
    if (Block && WmipHasStaticNames(Block))
    {
        if (Wnode->Flags & WNODE_FLAG_ALL_DATA)
        {
            Start = WMIP_ALIGN(Wnode->BufferSize, sizeof(ULONG));
            Size = Start + Block->NameSpace;
            if ((Wnode->BufferSize >= (ULONG)FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength)) &&
                (Size <= MAXULONG))
            {
                Copy = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)Size, TAG_WMI_PROVIDER);
            }

            if (Copy)
            {
                RtlCopyMemory(Copy, Wnode, Wnode->BufferSize);
                WmipAppendStaticNames((PWNODE_ALL_DATA)Copy, (ULONG)Size, Block);
            }
        }
        else if (Wnode->BufferSize >= (ULONG)FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData))
        {
            Index = ((PWNODE_SINGLE_INSTANCE)Wnode)->InstanceIndex;
            if (Index < Block->InstanceCount)
            {
                Start = WMIP_ALIGN(Wnode->BufferSize, sizeof(USHORT));
                Size = Start + WmipStaticNameSize(Block, Index);
                if (Size <= MAXULONG)
                    Copy = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)Size, TAG_WMI_PROVIDER);
            }

            if (Copy)
            {
                RtlCopyMemory(Copy, Wnode, Wnode->BufferSize);
                Single = (PWNODE_SINGLE_INSTANCE)Copy;
                Single->OffsetInstanceName = (ULONG)Start;
                Copy->BufferSize = (ULONG)Start + WmipPutStaticName(Block, Index, (PUSHORT)((PUCHAR)Copy + Start));
                if (Block->Pdo)
                    Copy->Flags |= WNODE_FLAG_PDO_INSTANCE_NAMES;
            }
        }
    }

    ExReleaseFastMutex(&WmipProviderLock);

    if (Copy)
        return Copy;

    /* Without names the offsets must not point anywhere */
    if (Wnode->Flags & WNODE_FLAG_ALL_DATA)
    {
        if (Wnode->BufferSize >= FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceNameOffsets) + sizeof(ULONG))
            ((PWNODE_ALL_DATA)Wnode)->OffsetInstanceNameOffsets = 0;
    }
    else if (Wnode->BufferSize >= FIELD_OFFSET(WNODE_SINGLE_INSTANCE, OffsetInstanceName) + sizeof(ULONG))
    {
        ((PWNODE_SINGLE_INSTANCE)Wnode)->OffsetInstanceName = 0;
    }

    return Wnode;
}

static
PWMIP_PROVIDER
WmipReferenceProviderById(
    _In_ ULONG ProviderId)
{
    PWMIP_PROVIDER Provider;
    PLIST_ENTRY Entry;

    ExAcquireFastMutex(&WmipProviderLock);
    for (Entry = WmipProviderListHead.Flink; Entry != &WmipProviderListHead; Entry = Entry->Flink)
    {
        Provider = CONTAINING_RECORD(Entry, WMIP_PROVIDER, ProviderLink);
        if (Provider->ProviderId == ProviderId)
        {
            InterlockedIncrement(&Provider->RefCount);
            ExReleaseFastMutex(&WmipProviderLock);
            return Provider;
        }
    }
    ExReleaseFastMutex(&WmipProviderLock);

    return NULL;
}

/**
 * @brief Turns a WNODE_EVENT_REFERENCE into the data block instance it points at.
 *
 * The provider that fired it is queried for the target, growing the buffer once if it asks.
 */
static
PWNODE_HEADER
NTAPI
WmipResolveEventReference(
    _In_ PWNODE_EVENT_REFERENCE Reference)
{
    PWNODE_SINGLE_INSTANCE Query = NULL;
    PWMIP_PROVIDER Provider;
    ULONGLONG DataOffset, Size;
    ULONG_PTR Information;
    ULONG NameBytes = 0, Needed, Try;
    BOOLEAN ByIndex;
    NTSTATUS Status;

    if (Reference->WnodeHeader.BufferSize < FIELD_OFFSET(WNODE_EVENT_REFERENCE, TargetInstanceIndex) + sizeof(ULONG))
        return NULL;

    /* The name form is a counted string in place of the index */
    ByIndex = (Reference->WnodeHeader.Flags & WNODE_FLAG_STATIC_INSTANCE_NAMES) != 0;
    if (!ByIndex)
    {
        NameBytes = *(PUSHORT)Reference->TargetInstanceName & ~(sizeof(WCHAR) - 1);
        if (FIELD_OFFSET(WNODE_EVENT_REFERENCE, TargetInstanceName) + sizeof(USHORT) + NameBytes >
            Reference->WnodeHeader.BufferSize)
        {
            return NULL;
        }
    }

    DataOffset = WMIP_ALIGN(FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData) +
                            (ByIndex ? 0 : sizeof(USHORT) + NameBytes), 8);
    Size = DataOffset + Reference->TargetDataBlockSize;
    if (Size > MAXULONG)
        return NULL;

    Provider = WmipReferenceProviderById(Reference->WnodeHeader.ProviderId);
    if (!Provider)
        return NULL;

    for (Try = 0; Try < WMIP_EVENT_REFERENCE_TRIES; Try++)
    {
        Query = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)Size, TAG_WMI_PROVIDER);
        if (!Query)
            break;

        RtlZeroMemory(Query, (SIZE_T)Size);
        Query->WnodeHeader.BufferSize = (ULONG)Size;
        Query->WnodeHeader.ProviderId = Reference->WnodeHeader.ProviderId;
        Query->WnodeHeader.Version = Reference->WnodeHeader.Version;
        Query->WnodeHeader.Guid = Reference->TargetGuid;
        Query->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE |
                                   (ByIndex ? WNODE_FLAG_STATIC_INSTANCE_NAMES : 0);
        Query->DataBlockOffset = (ULONG)DataOffset;

        if (ByIndex)
        {
            Query->InstanceIndex = Reference->TargetInstanceIndex;
        }
        else
        {
            Query->OffsetInstanceName = FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData);
            *(PUSHORT)Query->VariableData = (USHORT)NameBytes;
            RtlCopyMemory(Query->VariableData + sizeof(USHORT), &Reference->TargetInstanceName[1], NameBytes);
        }

        Status = WmipSendIrp(Provider,
                             IRP_MN_QUERY_SINGLE_INSTANCE,
                             &Query->WnodeHeader.Guid,
                             (ULONG)Size,
                             Query,
                             &Information);
        if (NT_SUCCESS(Status) && !(Query->WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL))
        {
            Query->WnodeHeader.Flags |= (Reference->WnodeHeader.Flags & WNODE_FLAG_SEVERITY_MASK) |
                                        WNODE_FLAG_EVENT_ITEM;
            break;
        }

        Needed = NT_SUCCESS(Status) ? ((PWNODE_TOO_SMALL)Query)->SizeNeeded : 0;
        ExFreePoolWithTag(Query, TAG_WMI_PROVIDER);
        Query = NULL;

        if (Needed <= Size)
            break;
        Size = Needed;
    }

    WmipDereferenceProvider(Provider);
    return (PWNODE_HEADER)Query;
}

/**
 * @brief Brings a fired event into the form consumers receive.
 *
 * Returns the event itself, a copy to release with WmipFreeEventCopy, or NULL to drop it.
 */
PWNODE_HEADER
NTAPI
WmipPrepareEvent(
    _In_ PWNODE_HEADER Wnode)
{
    PWNODE_HEADER Resolved = Wnode;
    PWNODE_HEADER Named;

    PAGED_CODE();

    if (Wnode->Flags & WNODE_FLAG_EVENT_REFERENCE)
    {
        Resolved = WmipResolveEventReference((PWNODE_EVENT_REFERENCE)Wnode);
        if (!Resolved)
            return NULL;
    }

    if (!(Resolved->Flags & WNODE_FLAG_STATIC_INSTANCE_NAMES))
        return Resolved;

    Named = WmipAddEventNames(Resolved);
    if ((Named != Resolved) && (Resolved != Wnode))
        WmipFreeEventCopy(Resolved);

    return Named;
}

VOID
NTAPI
WmipFreeEventCopy(
    _In_ PWNODE_HEADER Wnode)
{
    ExFreePoolWithTag(Wnode, TAG_WMI_PROVIDER);
}

#ifdef _WIN64
/* Drivers stamp WNODE headers with this, at up to DISPATCH_LEVEL */
ULONG
NTAPI
IoWMIDeviceObjectToProviderId(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PWMIP_PROVIDER Provider;
    PLIST_ENTRY Entry;
    ULONG ProviderId = 0;
    KIRQL OldIrql;

    KeAcquireSpinLock(&WmipProviderIdLock, &OldIrql);
    for (Entry = WmipProviderListHead.Flink; Entry != &WmipProviderListHead; Entry = Entry->Flink)
    {
        Provider = CONTAINING_RECORD(Entry, WMIP_PROVIDER, ProviderLink);
        if (Provider->DeviceObject == DeviceObject)
        {
            ProviderId = Provider->ProviderId;
            break;
        }
    }
    KeReleaseSpinLock(&WmipProviderIdLock, OldIrql);

    return ProviderId;
}
#endif
