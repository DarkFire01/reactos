/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectComposition connections, channels and compositor batches
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/dcompprotocol.h>

DBG_DEFAULT_CHANNEL(UserMisc);

#define TAG_DCOMP 'pmcD'

/* Handles a client may use for resources in one channel */
#define DCOMP_MAX_RESOURCES 65536

/* The compositor connection's handle; client connections get their process id */
#define DCOMP_COMPOSITOR_CONNECTION ((HANDLE)(ULONG_PTR)1)

typedef struct _DCOMP_RESOURCE
{
    ULONG Type;
    ULONG CompositorId;
} DCOMP_RESOURCE, *PDCOMP_RESOURCE;

/* Commands for one compositor resource, kept in order until commit */
typedef struct _DCOMP_GROUP
{
    ULONG Resource;
    ULONG Size;
} DCOMP_GROUP, *PDCOMP_GROUP;

typedef struct _DCOMP_CHANNEL
{
    LIST_ENTRY Link;
    ULONG Id;
    PEPROCESS Process;
    PVOID UserBuffer;
    SIZE_T BufferSize;
    PDCOMP_RESOURCE Resources;
    ULONG ResourceCount;
    PUCHAR Pending;
    ULONG PendingSize;
    ULONG PendingCapacity;
    PDCOMP_GROUP LastGroup;
    ULONG BatchId;
    ULONG64 ConnectionId[2];
} DCOMP_CHANNEL, *PDCOMP_CHANNEL;

/* A node waiting for the compositor; Data holds groups for resource commands */
typedef struct _DCOMP_QUEUED
{
    LIST_ENTRY Link;
    ULONG Type;
    ULONG Channel;
    ULONG Value;
    ULONG Size;
    UCHAR Data[ANYSIZE_ARRAY];
} DCOMP_QUEUED, *PDCOMP_QUEUED;

static LIST_ENTRY gDcompChannels = { &gDcompChannels, &gDcompChannels };
static LIST_ENTRY gDcompQueue = { &gDcompQueue, &gDcompQueue };
static ULONG gDcompNextChannel = 1;
static ULONG gDcompNextResource = 1;
static PEPROCESS gDcompCompositor = NULL;
static PKEVENT gDcompWorkEvent = NULL;
static PVOID gDcompUserList = NULL;

/* The properties every kind of visual has */
static
BOOLEAN
DcompIsValidVisualProperty(
    _In_ ULONG Tag,
    _In_ ULONG Property)
{
    switch (Property)
    {
        case DCOMP_VISUAL_OFFSET_X:
        case DCOMP_VISUAL_OFFSET_Y:
        case DCOMP_VISUAL_OPACITY:
            return Tag == DCOMP_CMD_SET_FLOAT_PROPERTY;
        case DCOMP_VISUAL_INSERT_CHILD:
        case DCOMP_VISUAL_REMOVE_CHILD:
        case DCOMP_VISUAL_CONTENT:
            return Tag == DCOMP_CMD_SET_REFERENCE_PROPERTY;
        case DCOMP_VISUAL_SIZE:
            return Tag == DCOMP_CMD_SET_INTEGER_PROPERTY;
    }
    return FALSE;
}

static
BOOLEAN
DcompIsValidProperty(
    _In_ ULONG Type,
    _In_ ULONG Tag,
    _In_ ULONG Property)
{
    switch (Type)
    {
        case DCOMP_RESOURCE_WINDOW_NODE:
            if (Property == DCOMP_WINDOW_NODE_SPRITE_BITMAP)
                return Tag == DCOMP_CMD_SET_REFERENCE_PROPERTY;
            if (Property == DCOMP_WINDOW_NODE_WINDOW)
                return Tag == DCOMP_CMD_SET_INTEGER_PROPERTY;
            return DcompIsValidVisualProperty(Tag, Property);

        case DCOMP_RESOURCE_SPRITE_VISUAL:
            if (Property == DCOMP_SPRITE_VISUAL_BRUSH)
                return Tag == DCOMP_CMD_SET_REFERENCE_PROPERTY;
            return DcompIsValidVisualProperty(Tag, Property);

        case DCOMP_RESOURCE_VISUAL:
            return DcompIsValidVisualProperty(Tag, Property);

        case DCOMP_RESOURCE_GDI_SPRITE_BITMAP:
            switch (Property)
            {
                case DCOMP_SPRITE_BITMAP_DIRTY_RECTS:
                case DCOMP_SPRITE_BITMAP_MARGINS:
                    return Tag == DCOMP_CMD_SET_BUFFER_PROPERTY;
                case DCOMP_SPRITE_BITMAP_FORMAT:
                case DCOMP_SPRITE_BITMAP_SURFACE:
                    return Tag == DCOMP_CMD_SET_INTEGER_PROPERTY;
            }
            return FALSE;

        case DCOMP_RESOURCE_COLOR_BRUSH:
            return Property == DCOMP_COLOR_BRUSH_COLOR && Tag == DCOMP_CMD_SET_BUFFER_PROPERTY;

        case DCOMP_RESOURCE_SURFACE_BRUSH:
            if (Property == DCOMP_SURFACE_BRUSH_SURFACE)
                return Tag == DCOMP_CMD_SET_REFERENCE_PROPERTY;
            return (Property == DCOMP_SURFACE_BRUSH_SOURCE_RECT || Property == DCOMP_SURFACE_BRUSH_NINE_GRID) &&
                   Tag == DCOMP_CMD_SET_BUFFER_PROPERTY;

        case DCOMP_RESOURCE_BACKDROP_BRUSH:
            if (Property == DCOMP_BACKDROP_BRUSH_BLUR)
                return Tag == DCOMP_CMD_SET_FLOAT_PROPERTY;
            return Property == DCOMP_BACKDROP_BRUSH_TINT && Tag == DCOMP_CMD_SET_BUFFER_PROPERTY;

        case DCOMP_RESOURCE_BITMAP_SOURCE:
            if (Property == DCOMP_BITMAP_SOURCE_SECTION)
                return Tag == DCOMP_CMD_SET_INTEGER_PROPERTY;
            return Property == DCOMP_BITMAP_SOURCE_DESC && Tag == DCOMP_CMD_SET_BUFFER_PROPERTY;

        case DCOMP_RESOURCE_HWND_TARGET:
            switch (Property)
            {
                case DCOMP_TARGET_WINDOW:
                case DCOMP_TARGET_TOPMOST:
                    return Tag == DCOMP_CMD_SET_INTEGER_PROPERTY;
                case DCOMP_TARGET_ROOT:
                    return Tag == DCOMP_CMD_SET_REFERENCE_PROPERTY;
            }
            return FALSE;
    }
    return FALSE;
}

static
BOOLEAN
DcompIsKnownType(
    _In_ ULONG Type)
{
    return Type == DCOMP_RESOURCE_VISUAL ||
           Type == DCOMP_RESOURCE_SPRITE_VISUAL ||
           Type == DCOMP_RESOURCE_HWND_TARGET ||
           Type == DCOMP_RESOURCE_WINDOW_NODE ||
           Type == DCOMP_RESOURCE_GDI_SPRITE_BITMAP ||
           Type == DCOMP_RESOURCE_COLOR_BRUSH ||
           Type == DCOMP_RESOURCE_SURFACE_BRUSH ||
           Type == DCOMP_RESOURCE_BACKDROP_BRUSH ||
           Type == DCOMP_RESOURCE_BITMAP_SOURCE;
}

/* Windows and their surfaces are only composed by the DWM */
static
BOOLEAN
DcompIsDwmOnlyType(
    _In_ ULONG Type)
{
    return Type == DCOMP_RESOURCE_WINDOW_NODE ||
           Type == DCOMP_RESOURCE_GDI_SPRITE_BITMAP ||
           Type == DCOMP_RESOURCE_BITMAP_SOURCE;
}

static
PDCOMP_CHANNEL
DcompLookupChannel(
    _In_ HANDLE Handle)
{
    PLIST_ENTRY Entry;
    ULONG Id = HandleToUlong(Handle);

    for (Entry = gDcompChannels.Flink; Entry != &gDcompChannels; Entry = Entry->Flink)
    {
        PDCOMP_CHANNEL Channel = CONTAINING_RECORD(Entry, DCOMP_CHANNEL, Link);
        if (Channel->Id == Id && Channel->Process == PsGetCurrentProcess())
            return Channel;
    }
    return NULL;
}

static
PDCOMP_RESOURCE
DcompLookupResource(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Handle)
{
    if (Handle == 0 || Handle > Channel->ResourceCount)
        return NULL;
    if (!Channel->Resources[Handle - 1].Type)
        return NULL;
    return &Channel->Resources[Handle - 1];
}

static
VOID
DcompSignalCompositor(VOID)
{
    if (gDcompWorkEvent)
        KeSetEvent(gDcompWorkEvent, IO_NO_INCREMENT, FALSE);
}

static
NTSTATUS
DcompQueueNode(
    _In_ ULONG Type,
    _In_ ULONG ChannelId,
    _In_ ULONG Value,
    _In_reads_bytes_opt_(Size) const VOID* Data,
    _In_ ULONG Size)
{
    PDCOMP_QUEUED Node;

    if (!gDcompCompositor)
        return STATUS_SUCCESS;

    Node = ExAllocatePoolWithTag(PagedPool, FIELD_OFFSET(DCOMP_QUEUED, Data[Size]), TAG_DCOMP);
    if (!Node)
        return STATUS_NO_MEMORY;

    Node->Type = Type;
    Node->Channel = ChannelId;
    Node->Value = Value;
    Node->Size = Size;
    if (Size)
        RtlCopyMemory(Node->Data, Data, Size);
    InsertTailList(&gDcompQueue, &Node->Link);
    DcompSignalCompositor();
    return STATUS_SUCCESS;
}

/* Appends one compositor command for a resource, opening a new group when the resource changes */
static
NTSTATUS
DcompAppend(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG CompositorId,
    _In_ ULONG Command,
    _In_reads_bytes_opt_(PayloadSize) const VOID* Payload,
    _In_ ULONG PayloadSize,
    _In_reads_bytes_opt_(ExtraSize) const VOID* Extra,
    _In_ ULONG ExtraSize)
{
    ULONG RecordSize = ALIGN_UP_BY(2 * sizeof(ULONG) + PayloadSize + ExtraSize, sizeof(ULONG));
    BOOLEAN NewGroup = !Channel->LastGroup || Channel->LastGroup->Resource != CompositorId;
    ULONG Needed = Channel->PendingSize + RecordSize + (NewGroup ? sizeof(DCOMP_GROUP) : 0);
    PULONG Record;

    if (Needed > Channel->PendingCapacity)
    {
        ULONG Capacity = max(Needed, Channel->PendingCapacity * 2);
        PUCHAR Grown = ExAllocatePoolWithTag(PagedPool, max(Capacity, PAGE_SIZE), TAG_DCOMP);
        if (!Grown)
            return STATUS_NO_MEMORY;
        if (Channel->Pending)
        {
            RtlCopyMemory(Grown, Channel->Pending, Channel->PendingSize);
            if (Channel->LastGroup)
                Channel->LastGroup = (PDCOMP_GROUP)(Grown + ((PUCHAR)Channel->LastGroup - Channel->Pending));
            ExFreePoolWithTag(Channel->Pending, TAG_DCOMP);
        }
        Channel->Pending = Grown;
        Channel->PendingCapacity = max(Capacity, PAGE_SIZE);
    }

    if (NewGroup)
    {
        Channel->LastGroup = (PDCOMP_GROUP)(Channel->Pending + Channel->PendingSize);
        Channel->LastGroup->Resource = CompositorId;
        Channel->LastGroup->Size = 0;
        Channel->PendingSize += sizeof(DCOMP_GROUP);
    }

    Record = (PULONG)(Channel->Pending + Channel->PendingSize);
    RtlZeroMemory(Record, RecordSize);
    Record[0] = RecordSize;
    Record[1] = Command;
    if (PayloadSize)
        RtlCopyMemory(&Record[2], Payload, PayloadSize);
    if (ExtraSize)
        RtlCopyMemory((PUCHAR)&Record[2] + PayloadSize, Extra, ExtraSize);

    Channel->PendingSize += RecordSize;
    Channel->LastGroup->Size += RecordSize;
    return STATUS_SUCCESS;
}

static
NTSTATUS
DcompCreateResource(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_CREATE* Command)
{
    ULONG Payload[2];

    if (Command->External || !DcompIsKnownType(Command->Type))
        return STATUS_NOT_SUPPORTED;
    if (DcompIsDwmOnlyType(Command->Type) && !IntDwmIsCurrentProcess())
        return STATUS_ACCESS_DENIED;
    if (Command->Resource == 0 || Command->Resource > DCOMP_MAX_RESOURCES)
        return STATUS_INVALID_PARAMETER;

    if (Command->Resource > Channel->ResourceCount)
    {
        ULONG Count = max(Command->Resource, Channel->ResourceCount * 2);
        PDCOMP_RESOURCE Grown = ExAllocatePoolWithTag(PagedPool, Count * sizeof(DCOMP_RESOURCE), TAG_DCOMP);
        if (!Grown)
            return STATUS_NO_MEMORY;
        RtlZeroMemory(Grown, Count * sizeof(DCOMP_RESOURCE));
        if (Channel->Resources)
        {
            RtlCopyMemory(Grown, Channel->Resources, Channel->ResourceCount * sizeof(DCOMP_RESOURCE));
            ExFreePoolWithTag(Channel->Resources, TAG_DCOMP);
        }
        Channel->Resources = Grown;
        Channel->ResourceCount = Count;
    }

    if (Channel->Resources[Command->Resource - 1].Type)
        return STATUS_OBJECT_NAME_COLLISION;

    Channel->Resources[Command->Resource - 1].Type = Command->Type;
    Channel->Resources[Command->Resource - 1].CompositorId = gDcompNextResource++;

    Payload[0] = Command->Type;
    Payload[1] = Channel->Id;
    return DcompAppend(Channel,
                       Channel->Resources[Command->Resource - 1].CompositorId,
                       DCOMP_MILCMD_CREATE,
                       Payload,
                       sizeof(Payload),
                       NULL,
                       0);
}

/* Validates one client command and turns it into compositor commands; returns its size or 0 */
static
ULONG
DcompProcessCommand(
    _In_ PDCOMP_CHANNEL Channel,
    _In_reads_bytes_(Remaining) const UCHAR* Record,
    _In_ ULONG Remaining,
    _Out_ NTSTATUS* Status)
{
    ULONG Tag = *(const ULONG*)Record;
    PDCOMP_RESOURCE Resource;
    ULONG Payload[4];

    *Status = STATUS_INVALID_PARAMETER;

    switch (Tag)
    {
        case DCOMP_CMD_CREATE_RESOURCE:
            if (Remaining < sizeof(DCOMP_CMD_CREATE))
                return 0;
            *Status = DcompCreateResource(Channel, (const DCOMP_CMD_CREATE*)Record);
            return sizeof(DCOMP_CMD_CREATE);

        case DCOMP_CMD_RELEASE_RESOURCE:
        {
            const DCOMP_CMD_RELEASE* Command = (const DCOMP_CMD_RELEASE*)Record;
            if (Remaining < sizeof(*Command) || !(Resource = DcompLookupResource(Channel, Command->Resource)))
                return 0;
            *Status = DcompAppend(Channel, Resource->CompositorId, DCOMP_MILCMD_RELEASE, NULL, 0, NULL, 0);
            Resource->Type = 0;
            return sizeof(*Command);
        }

        case DCOMP_CMD_SET_INTEGER_PROPERTY:
        {
            const DCOMP_CMD_SET_INTEGER* Command = (const DCOMP_CMD_SET_INTEGER*)Record;
            if (Remaining < sizeof(*Command) || !(Resource = DcompLookupResource(Channel, Command->Resource)))
                return 0;
            if (!DcompIsValidProperty(Resource->Type, Tag, Command->Property))
                return 0;
            Payload[0] = Command->Property;
            Payload[1] = 0;
            *Status = DcompAppend(Channel, Resource->CompositorId, DCOMP_MILCMD_SET_INTEGER,
                                  Payload, 2 * sizeof(ULONG), &Command->Value, sizeof(Command->Value));
            return sizeof(*Command);
        }

        case DCOMP_CMD_SET_FLOAT_PROPERTY:
        {
            const DCOMP_CMD_SET_FLOAT* Command = (const DCOMP_CMD_SET_FLOAT*)Record;
            if (Remaining < sizeof(*Command) || !(Resource = DcompLookupResource(Channel, Command->Resource)))
                return 0;
            if (!DcompIsValidProperty(Resource->Type, Tag, Command->Property))
                return 0;
            Payload[0] = Command->Property;
            *Status = DcompAppend(Channel, Resource->CompositorId, DCOMP_MILCMD_SET_FLOAT,
                                  Payload, sizeof(ULONG), &Command->Value, sizeof(Command->Value));
            return sizeof(*Command);
        }

        case DCOMP_CMD_SET_REFERENCE_PROPERTY:
        {
            const DCOMP_CMD_SET_REFERENCE* Command = (const DCOMP_CMD_SET_REFERENCE*)Record;
            PDCOMP_RESOURCE Target = NULL;
            if (Remaining < sizeof(*Command) || !(Resource = DcompLookupResource(Channel, Command->Resource)))
                return 0;
            if (!DcompIsValidProperty(Resource->Type, Tag, Command->Property))
                return 0;
            if (Command->Value && !(Target = DcompLookupResource(Channel, Command->Value)))
                return 0;
            Payload[0] = Command->Property;
            Payload[1] = Target ? Target->CompositorId : 0;
            *Status = DcompAppend(Channel, Resource->CompositorId, DCOMP_MILCMD_SET_REFERENCE,
                                  Payload, 2 * sizeof(ULONG), NULL, 0);
            return sizeof(*Command);
        }

        case DCOMP_CMD_SET_BUFFER_PROPERTY:
        {
            const DCOMP_CMD_SET_BUFFER* Command = (const DCOMP_CMD_SET_BUFFER*)Record;
            ULONG Size;
            if (Remaining < sizeof(*Command) || !(Resource = DcompLookupResource(Channel, Command->Resource)))
                return 0;
            Size = sizeof(*Command) + ALIGN_UP_BY(Command->Size, sizeof(ULONG));
            if (Command->Size > Remaining || Size > Remaining)
                return 0;
            if (!DcompIsValidProperty(Resource->Type, Tag, Command->Property))
                return 0;
            Payload[0] = Command->Property;
            Payload[1] = Command->Size;
            *Status = DcompAppend(Channel, Resource->CompositorId, DCOMP_MILCMD_SET_BUFFER,
                                  Payload, 2 * sizeof(ULONG), Command + 1, Command->Size);
            return Size;
        }

        case DCOMP_CMD_SET_HANDLE_PROPERTY:
            *Status = STATUS_NOT_SUPPORTED;
            return 0;
    }
    return 0;
}

static
VOID
DcompDestroyChannel(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ BOOLEAN FreeUserBuffer)
{
    SIZE_T Size = 0;

    RemoveEntryList(&Channel->Link);
    if (gDcompCompositor)
        DcompQueueNode(DCOMP_BATCH_CLOSE_CHANNEL, Channel->Id, 0, NULL, 0);

    if (FreeUserBuffer && Channel->UserBuffer)
        ZwFreeVirtualMemory(NtCurrentProcess(), &Channel->UserBuffer, &Size, MEM_RELEASE);
    if (Channel->Resources)
        ExFreePoolWithTag(Channel->Resources, TAG_DCOMP);
    if (Channel->Pending)
        ExFreePoolWithTag(Channel->Pending, TAG_DCOMP);
    ExFreePoolWithTag(Channel, TAG_DCOMP);
}

static
VOID
DcompResetCompositor(VOID)
{
    while (!IsListEmpty(&gDcompQueue))
    {
        PDCOMP_QUEUED Node = CONTAINING_RECORD(RemoveHeadList(&gDcompQueue), DCOMP_QUEUED, Link);
        ExFreePoolWithTag(Node, TAG_DCOMP);
    }
    if (gDcompWorkEvent)
        ObDereferenceObject(gDcompWorkEvent);
    gDcompWorkEvent = NULL;
    gDcompCompositor = NULL;
    gDcompUserList = NULL;
}

/* Process teardown: channels go away with their owner, and so does the compositor connection */
VOID
FASTCALL
IntDcompProcessCleanup(
    _In_ PEPROCESS Process)
{
    PLIST_ENTRY Entry = gDcompChannels.Flink;

    while (Entry != &gDcompChannels)
    {
        PDCOMP_CHANNEL Channel = CONTAINING_RECORD(Entry, DCOMP_CHANNEL, Link);
        Entry = Entry->Flink;
        if (Channel->Process == Process)
            DcompDestroyChannel(Channel, FALSE);
    }

    if (gDcompCompositor == Process)
        DcompResetCompositor();
}

NTSTATUS
APIENTRY
NtDCompositionCreateConnection(
    _In_ ULONG Compositor,
    _In_opt_ PVOID ClientInfo,
    _Out_ PHANDLE Connection)
{
    DCOMP_COMPOSITOR_INFO Info;
    PKEVENT Event;
    HANDLE Result;
    NTSTATUS Status = STATUS_SUCCESS;

    UserEnterExclusive();

    if (!Compositor)
    {
        Result = PsGetCurrentProcessId();
    }
    else
    {
        if (!IntDwmIsCurrentProcess())
        {
            Status = STATUS_ACCESS_DENIED;
            goto Exit;
        }
        if (gDcompCompositor)
        {
            Status = STATUS_OBJECT_NAME_COLLISION;
            goto Exit;
        }

        _SEH2_TRY
        {
            ProbeForRead(ClientInfo, sizeof(Info), sizeof(ULONG));
            Info = *(PDCOMP_COMPOSITOR_INFO)ClientInfo;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status))
            goto Exit;

        Status = ObReferenceObjectByHandle(Info.WorkEvent,
                                           EVENT_MODIFY_STATE,
                                           *ExEventObjectType,
                                           UserMode,
                                           (PVOID*)&Event,
                                           NULL);
        if (!NT_SUCCESS(Status))
            goto Exit;

        gDcompCompositor = PsGetCurrentProcess();
        gDcompWorkEvent = Event;
        Result = DCOMP_COMPOSITOR_CONNECTION;
    }

    _SEH2_TRY
    {
        ProbeForWrite(Connection, sizeof(*Connection), sizeof(ULONG));
        *Connection = Result;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

Exit:
    UserLeave();
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionCreateChannel(
    _Out_ PHANDLE ChannelHandle,
    _Inout_ PULONG SectionSize,
    _Out_ PVOID* SharedSection)
{
    PDCOMP_CHANNEL Channel = NULL;
    SIZE_T Size;
    ULONG Requested = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    UserEnterExclusive();

    if (!gDcompCompositor)
    {
        Status = STATUS_DEVICE_NOT_READY;
        goto Exit;
    }

    _SEH2_TRY
    {
        ProbeForWrite(SectionSize, sizeof(*SectionSize), sizeof(ULONG));
        Requested = *SectionSize;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        goto Exit;

    Channel = ExAllocatePoolWithTag(PagedPool, sizeof(*Channel), TAG_DCOMP);
    if (!Channel)
    {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }
    RtlZeroMemory(Channel, sizeof(*Channel));

    Size = ALIGN_UP_BY(min(max(Requested, PAGE_SIZE), DCOMP_CHANNEL_BUFFER_SIZE), PAGE_SIZE);
    Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &Channel->UserBuffer, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Channel->BufferSize = Size;
    Channel->Process = PsGetCurrentProcess();
    Channel->Id = gDcompNextChannel++;

    _SEH2_TRY
    {
        ProbeForWrite(ChannelHandle, sizeof(*ChannelHandle), sizeof(ULONG));
        ProbeForWrite(SharedSection, sizeof(*SharedSection), sizeof(ULONG));
        *ChannelHandle = UlongToHandle(Channel->Id);
        *SectionSize = (ULONG)Size;
        *SharedSection = Channel->UserBuffer;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        goto Exit;

    InsertTailList(&gDcompChannels, &Channel->Link);
    DcompQueueNode(DCOMP_BATCH_OPEN_CHANNEL, Channel->Id, HandleToUlong(PsGetCurrentProcessId()), NULL, 0);
    Channel = NULL;

Exit:
    if (Channel)
    {
        Size = 0;
        if (Channel->UserBuffer)
            ZwFreeVirtualMemory(NtCurrentProcess(), &Channel->UserBuffer, &Size, MEM_RELEASE);
        ExFreePoolWithTag(Channel, TAG_DCOMP);
    }
    UserLeave();
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionDestroyChannel(
    _In_ HANDLE ChannelHandle)
{
    PDCOMP_CHANNEL Channel;
    NTSTATUS Status = STATUS_SUCCESS;

    UserEnterExclusive();
    Channel = DcompLookupChannel(ChannelHandle);
    if (Channel)
        DcompDestroyChannel(Channel, TRUE);
    else
        Status = STATUS_INVALID_HANDLE;
    UserLeave();
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionProcessChannelBatchBuffer(
    _In_ HANDLE ChannelHandle,
    _In_ ULONG Length,
    _Out_ PULONG Consumed,
    _Out_ PBOOLEAN MoreWork)
{
    PDCOMP_CHANNEL Channel;
    PUCHAR Copy = NULL;
    ULONG Offset = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    UserEnterExclusive();

    Channel = DcompLookupChannel(ChannelHandle);
    if (!Channel)
    {
        Status = STATUS_INVALID_HANDLE;
        goto Exit;
    }
    if (Length > Channel->BufferSize)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    /* Parse a private copy so the client cannot change records under validation */
    if (Length)
    {
        Copy = ExAllocatePoolWithTag(PagedPool, Length, TAG_DCOMP);
        if (!Copy)
        {
            Status = STATUS_NO_MEMORY;
            goto Exit;
        }
        _SEH2_TRY
        {
            RtlCopyMemory(Copy, Channel->UserBuffer, Length);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status))
            goto Exit;
    }

    while (Length - Offset >= sizeof(ULONG))
    {
        ULONG Used = DcompProcessCommand(Channel, Copy + Offset, Length - Offset, &Status);
        if (!Used || !NT_SUCCESS(Status))
        {
            if (NT_SUCCESS(Status))
                Status = STATUS_INVALID_PARAMETER;
            ERR("DComp channel %lu: bad command at offset %lu (0x%08lx)\n", Channel->Id, Offset, Status);
            break;
        }
        Offset += Used;
    }

    _SEH2_TRY
    {
        ProbeForWrite(Consumed, sizeof(*Consumed), sizeof(ULONG));
        ProbeForWrite(MoreWork, sizeof(*MoreWork), sizeof(BOOLEAN));
        *Consumed = Offset;
        *MoreWork = FALSE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

Exit:
    if (Copy)
        ExFreePoolWithTag(Copy, TAG_DCOMP);
    UserLeave();
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionCommitChannel(
    _In_ HANDLE ChannelHandle,
    _Out_opt_ PULONG BatchId,
    _Out_opt_ PBOOLEAN Pending,
    _In_ ULONG Flags,
    _In_opt_ HANDLE SyncObject,
    _In_opt_ PVOID Reserved1,
    _In_opt_ PVOID Reserved2,
    _In_ ULONG Reserved3)
{
    PDCOMP_CHANNEL Channel;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Id = 0;

    UserEnterExclusive();

    Channel = DcompLookupChannel(ChannelHandle);
    if (!Channel)
    {
        Status = STATUS_INVALID_HANDLE;
        goto Exit;
    }

    if (Channel->PendingSize)
    {
        Status = DcompQueueNode(DCOMP_BATCH_RESOURCE_COMMANDS, Channel->Id, 0, Channel->Pending, Channel->PendingSize);
        if (!NT_SUCCESS(Status))
            goto Exit;
        Channel->PendingSize = 0;
        Channel->LastGroup = NULL;
    }
    Id = ++Channel->BatchId;

    _SEH2_TRY
    {
        if (BatchId)
        {
            ProbeForWrite(BatchId, sizeof(*BatchId), sizeof(ULONG));
            *BatchId = Id;
        }
        if (Pending)
        {
            ProbeForWrite(Pending, sizeof(*Pending), sizeof(BOOLEAN));
            *Pending = FALSE;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

Exit:
    UserLeave();
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionWaitForChannel(
    _In_ HANDLE ChannelHandle,
    _In_ BOOL WaitAll)
{
    NTSTATUS Status;

    UserEnterShared();
    Status = DcompLookupChannel(ChannelHandle) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
    UserLeave();
    return Status;
}

NTSTATUS
APIENTRY
NtDCompositionSetChannelConnectionId(
    _In_ HANDLE ChannelHandle,
    _In_ ULONG Slot,
    _In_ ULONG64 ConnectionId)
{
    PDCOMP_CHANNEL Channel;
    NTSTATUS Status = STATUS_SUCCESS;

    UserEnterExclusive();
    Channel = DcompLookupChannel(ChannelHandle);
    if (Channel)
        Channel->ConnectionId[Slot ? 1 : 0] = ConnectionId;
    else
        Status = STATUS_INVALID_HANDLE;
    UserLeave();
    return Status;
}

/* Hands the compositor everything queued since its last call, laid out in its own address space */
NTSTATUS
APIENTRY
NtDCompositionGetConnectionBatch(
    _In_ HANDLE Connection,
    _In_opt_ PVOID Context,
    _Out_ PVOID* ListHead)
{
    PLIST_ENTRY Entry;
    SIZE_T Total = 0;
    SIZE_T Size = 0;
    PUCHAR Base = NULL;
    PDCOMP_BATCH_NODE Previous = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    UserEnterExclusive();

    if (Connection != DCOMP_COMPOSITOR_CONNECTION || PsGetCurrentProcess() != gDcompCompositor)
    {
        Status = STATUS_ACCESS_DENIED;
        goto Exit;
    }

    if (gDcompUserList)
    {
        ZwFreeVirtualMemory(NtCurrentProcess(), &gDcompUserList, &Size, MEM_RELEASE);
        gDcompUserList = NULL;
    }

    for (Entry = gDcompQueue.Flink; Entry != &gDcompQueue; Entry = Entry->Flink)
    {
        PDCOMP_QUEUED Node = CONTAINING_RECORD(Entry, DCOMP_QUEUED, Link);
        if (Node->Type != DCOMP_BATCH_RESOURCE_COMMANDS)
        {
            Total += sizeof(DCOMP_BATCH_NODE);
            continue;
        }
        /* Every group becomes a node followed by its commands */
        for (ULONG Offset = 0; Offset < Node->Size;)
        {
            PDCOMP_GROUP Group = (PDCOMP_GROUP)(Node->Data + Offset);
            Total += sizeof(DCOMP_BATCH_NODE) + Group->Size;
            Offset += sizeof(*Group) + Group->Size;
        }
    }

    if (Total)
    {
        Size = Total;
        Status = ZwAllocateVirtualMemory(NtCurrentProcess(), (PVOID*)&Base, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
            goto Exit;
        gDcompUserList = Base;
    }

    _SEH2_TRY
    {
        PUCHAR Cursor = Base;

        while (!IsListEmpty(&gDcompQueue))
        {
            PDCOMP_QUEUED Node = CONTAINING_RECORD(RemoveHeadList(&gDcompQueue), DCOMP_QUEUED, Link);
            ULONG Offset = 0;

            do
            {
                PDCOMP_BATCH_NODE Out = (PDCOMP_BATCH_NODE)Cursor;
                RtlZeroMemory(Out, sizeof(*Out));
                Out->Type = Node->Type;
                Out->Channel = Node->Channel;
                Out->Value = Node->Value;
                Cursor += sizeof(*Out);

                if (Node->Type == DCOMP_BATCH_RESOURCE_COMMANDS)
                {
                    PDCOMP_GROUP Group = (PDCOMP_GROUP)(Node->Data + Offset);
                    Out->Channel = Group->Resource;
                    Out->Value = Group->Size;
                    Out->Commands = (ULONG64)(ULONG_PTR)Cursor;
                    RtlCopyMemory(Cursor, Group + 1, Group->Size);
                    Cursor += Group->Size;
                    Offset += sizeof(*Group) + Group->Size;
                }

                if (Previous)
                    Previous->Next = (ULONG64)(ULONG_PTR)Out;
                Previous = Out;
            } while (Node->Type == DCOMP_BATCH_RESOURCE_COMMANDS && Offset < Node->Size);

            ExFreePoolWithTag(Node, TAG_DCOMP);
        }

        ProbeForWrite(ListHead, sizeof(*ListHead), sizeof(ULONG));
        *ListHead = Base;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

Exit:
    UserLeave();
    return Status;
}
