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

/* Largest shared section an animation's segments may live in */
#define DCOMP_MAX_SHARED_SECTION (16 * 1024 * 1024)

/* The compositor connection's handle; client connections get their process id */
#define DCOMP_COMPOSITOR_CONNECTION ((HANDLE)(ULONG_PTR)1)

/* What a resource keeps so its commands to the compositor carry whole values */
typedef struct _DCOMP_RESOURCE
{
    ULONG Type;
    union
    {
        struct
        {
            FLOAT Offset[3];
            FLOAT Size[2];
        } Visual;
        struct
        {
            ULONG Section;
            ULONG Offset;
            ULONG Size;
            LONGLONG Begin;
            LONGLONG End;
        } Animation;
        struct
        {
            PVOID Section;
            ULONG Size;
        } Shared;
        DCOMP_CACHED_IMAGE_UPDATE CachedImage;
    };
} DCOMP_RESOURCE, *PDCOMP_RESOURCE;

/* A property an animation drives */
typedef struct _DCOMP_BINDING
{
    ULONG Target;
    ULONG Property;
    ULONG Animation;
} DCOMP_BINDING, *PDCOMP_BINDING;

typedef struct _DCOMP_CHANNEL
{
    LIST_ENTRY Link;
    ULONG Id;
    PEPROCESS Process;
    PVOID UserBuffer;
    SIZE_T BufferSize;
    PDCOMP_RESOURCE Resources;
    ULONG ResourceCount;
    PDCOMP_BINDING Bindings;
    ULONG BindingCount;
    ULONG BindingCapacity;
    PUCHAR Pending;
    ULONG PendingSize;
    ULONG PendingCapacity;
    ULONG BatchId;
    ULONG64 ConnectionId[2];
} DCOMP_CHANNEL, *PDCOMP_CHANNEL;

/* A node waiting for the compositor; Data holds a channel's commands */
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
static PEPROCESS gDcompCompositor = NULL;
static PKEVENT gDcompWorkEvent = NULL;
static PVOID gDcompUserList = NULL;

static
BOOLEAN
DcompIsVisualType(
    _In_ ULONG Type)
{
    return Type == DCOMP_RESOURCE_VISUAL ||
           Type == DCOMP_RESOURCE_SPRITE_VISUAL ||
           Type == DCOMP_RESOURCE_WINDOW_NODE;
}

/* The concrete types this kernel accepts; the rest of Windows' enum is not composed yet */
static
BOOLEAN
DcompIsKnownType(
    _In_ ULONG Type)
{
    switch (Type)
    {
        case DCOMP_RESOURCE_ANIMATION:
        case DCOMP_RESOURCE_BACKDROP_BRUSH:
        case DCOMP_RESOURCE_BITMAP_RESOURCE:
        case DCOMP_RESOURCE_CACHED_VISUAL_IMAGE:
        case DCOMP_RESOURCE_COLOR_BRUSH:
        case DCOMP_RESOURCE_COMPOSITION_SURFACE_BITMAP:
        case DCOMP_RESOURCE_EFFECT_GROUP:
        case DCOMP_RESOURCE_GDI_SPRITE_BITMAP:
        case DCOMP_RESOURCE_MATRIX_TRANSFORM:
        case DCOMP_RESOURCE_MATRIX_TRANSFORM_3D:
        case DCOMP_RESOURCE_RECTANGLE_CLIP:
        case DCOMP_RESOURCE_ROTATE_TRANSFORM:
        case DCOMP_RESOURCE_ROTATE_TRANSFORM_3D:
        case DCOMP_RESOURCE_SCALE_TRANSFORM:
        case DCOMP_RESOURCE_SCALE_TRANSFORM_3D:
        case DCOMP_RESOURCE_SHARED_SECTION:
        case DCOMP_RESOURCE_SKEW_TRANSFORM:
        case DCOMP_RESOURCE_SPRITE_VISUAL:
        case DCOMP_RESOURCE_SURFACE_BRUSH:
        case DCOMP_RESOURCE_TRANSFORM_GROUP:
        case DCOMP_RESOURCE_TRANSFORM_3D_GROUP:
        case DCOMP_RESOURCE_TRANSLATE_TRANSFORM:
        case DCOMP_RESOURCE_TRANSLATE_TRANSFORM_3D:
        case DCOMP_RESOURCE_VISUAL:
        case DCOMP_RESOURCE_WINDOW_NODE:
        case DCOMP_RESOURCE_HWND_TARGET:
            return TRUE;
    }
    return FALSE;
}

/* Windows and their surfaces are only composed by the DWM */
static
BOOLEAN
DcompIsDwmOnlyType(
    _In_ ULONG Type)
{
    return Type == DCOMP_RESOURCE_WINDOW_NODE ||
           Type == DCOMP_RESOURCE_GDI_SPRITE_BITMAP ||
           Type == DCOMP_RESOURCE_BITMAP_RESOURCE ||
           Type == DCOMP_RESOURCE_CACHED_VISUAL_IMAGE;
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

/* The compositor indexes channels by id, so ids are kept small and reused */
static
ULONG
DcompAllocateChannelId(VOID)
{
    PLIST_ENTRY Entry;
    ULONG Id;

    for (Id = 1; Id < DCOMP_MAX_CHANNELS; Id++)
    {
        BOOLEAN Used = FALSE;
        for (Entry = gDcompChannels.Flink; Entry != &gDcompChannels; Entry = Entry->Flink)
        {
            if (CONTAINING_RECORD(Entry, DCOMP_CHANNEL, Link)->Id == Id)
            {
                Used = TRUE;
                break;
            }
        }
        if (!Used)
            return Id;
    }
    return 0;
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

/* Appends [size][command][resource][payload][extra] to the channel's batch */
static
NTSTATUS
DcompEmit(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Command,
    _In_ ULONG Resource,
    _In_reads_bytes_opt_(PayloadSize) const VOID* Payload,
    _In_ ULONG PayloadSize,
    _In_reads_bytes_opt_(ExtraSize) const VOID* Extra,
    _In_ ULONG ExtraSize)
{
    ULONG RecordSize = ALIGN_UP_BY(3 * sizeof(ULONG) + PayloadSize + ExtraSize, sizeof(ULONG));
    ULONG Needed = Channel->PendingSize + RecordSize;
    PULONG Record;

    if (Needed > Channel->PendingCapacity)
    {
        ULONG Capacity = max(max(Needed, Channel->PendingCapacity * 2), PAGE_SIZE);
        PUCHAR Grown = ExAllocatePoolWithTag(PagedPool, Capacity, TAG_DCOMP);
        if (!Grown)
            return STATUS_NO_MEMORY;
        if (Channel->Pending)
        {
            RtlCopyMemory(Grown, Channel->Pending, Channel->PendingSize);
            ExFreePoolWithTag(Channel->Pending, TAG_DCOMP);
        }
        Channel->Pending = Grown;
        Channel->PendingCapacity = Capacity;
    }

    Record = (PULONG)(Channel->Pending + Channel->PendingSize);
    RtlZeroMemory(Record, RecordSize);
    Record[0] = RecordSize;
    Record[1] = Command;
    Record[2] = Resource;
    if (PayloadSize)
        RtlCopyMemory(&Record[3], Payload, PayloadSize);
    if (ExtraSize)
        RtlCopyMemory((PUCHAR)&Record[3] + PayloadSize, Extra, ExtraSize);

    Channel->PendingSize += RecordSize;
    return STATUS_SUCCESS;
}

/* Drops the binding of Target's Property; the compositor stops animating it */
static
NTSTATUS
DcompUnbind(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Target,
    _In_ ULONG Property)
{
    ULONG Index;

    for (Index = 0; Index < Channel->BindingCount; Index++)
    {
        ULONG Animation = Channel->Bindings[Index].Animation;
        ULONG Payload[2];

        if (Channel->Bindings[Index].Target != Target || Channel->Bindings[Index].Property != Property)
            continue;

        Payload[0] = Target;
        Payload[1] = Property;
        Channel->Bindings[Index] = Channel->Bindings[--Channel->BindingCount];
        return DcompEmit(Channel, DCOMP_MILCMD_BASE_ANIMATION_REMOVE_BINDING, Animation,
                         Payload, sizeof(Payload), NULL, 0);
    }
    return STATUS_SUCCESS;
}

static
NTSTATUS
DcompBind(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_SET_ANIMATION* Command)
{
    PDCOMP_RESOURCE Animation = DcompLookupResource(Channel, Command->Animation);
    ULONG Payload[2];
    NTSTATUS Status;

    if (!DcompLookupResource(Channel, Command->Resource) || !Animation ||
        Animation->Type != DCOMP_RESOURCE_ANIMATION)
        return STATUS_INVALID_PARAMETER;

    Status = DcompUnbind(Channel, Command->Resource, Command->Property);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Channel->BindingCount == Channel->BindingCapacity)
    {
        ULONG Capacity = max(Channel->BindingCapacity * 2, 16);
        PDCOMP_BINDING Grown = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(*Grown), TAG_DCOMP);
        if (!Grown)
            return STATUS_NO_MEMORY;
        if (Channel->Bindings)
        {
            RtlCopyMemory(Grown, Channel->Bindings, Channel->BindingCount * sizeof(*Grown));
            ExFreePoolWithTag(Channel->Bindings, TAG_DCOMP);
        }
        Channel->Bindings = Grown;
        Channel->BindingCapacity = Capacity;
    }

    Channel->Bindings[Channel->BindingCount].Target = Command->Resource;
    Channel->Bindings[Channel->BindingCount].Property = Command->Property;
    Channel->Bindings[Channel->BindingCount].Animation = Command->Animation;
    Channel->BindingCount++;

    Payload[0] = Command->Resource;
    Payload[1] = Command->Property;
    return DcompEmit(Channel, DCOMP_MILCMD_BASE_ANIMATION_ADD_BINDING, Command->Animation,
                     Payload, sizeof(Payload), NULL, 0);
}

/* A released resource takes the bindings it is part of with it */
static
VOID
DcompForgetBindings(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Handle)
{
    ULONG Index = 0;

    while (Index < Channel->BindingCount)
    {
        if (Channel->Bindings[Index].Target == Handle || Channel->Bindings[Index].Animation == Handle)
            Channel->Bindings[Index] = Channel->Bindings[--Channel->BindingCount];
        else
            Index++;
    }
}

static
NTSTATUS
DcompEmitOffset(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Handle,
    _In_ PDCOMP_RESOURCE Resource)
{
    return DcompEmit(Channel, DCOMP_MILCMD_VISUAL_SET_OFFSET, Handle, Resource->Visual.Offset,
                     sizeof(Resource->Visual.Offset), NULL, 0);
}

static
NTSTATUS
DcompEmitSize(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Handle,
    _In_ PDCOMP_RESOURCE Resource)
{
    return DcompEmit(Channel, DCOMP_MILCMD_VISUAL_SET_SIZE, Handle, Resource->Visual.Size,
                     sizeof(Resource->Visual.Size), NULL, 0);
}

/* An animation reads its segments once it knows where they are */
static
NTSTATUS
DcompEmitPrimitives(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Handle,
    _In_ PDCOMP_RESOURCE Resource)
{
    ULONG Payload[3];

    if (!Resource->Animation.Section)
        return STATUS_SUCCESS;

    Payload[0] = Resource->Animation.Section;
    Payload[1] = Resource->Animation.Offset;
    Payload[2] = Resource->Animation.Size;
    return DcompEmit(Channel, DCOMP_MILCMD_ANIMATION_SET_PRIMITIVES, Handle, Payload, sizeof(Payload), NULL, 0);
}

static
NTSTATUS
DcompEmitBeginTime(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Handle,
    _In_ PDCOMP_RESOURCE Resource)
{
    LONGLONG Payload[2];

    Payload[0] = Resource->Animation.Begin;
    Payload[1] = Resource->Animation.End;
    return DcompEmit(Channel, DCOMP_MILCMD_ANIMATION_UPDATE_BEGIN_TIME, Handle, Payload, sizeof(Payload), NULL, 0);
}

static
NTSTATUS
DcompEmitCachedImage(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ ULONG Handle,
    _In_ PDCOMP_RESOURCE Resource)
{
    return DcompEmit(Channel, DCOMP_MILCMD_CACHED_VISUAL_IMAGE_UPDATE, Handle, &Resource->CachedImage,
                     sizeof(Resource->CachedImage), NULL, 0);
}

static
VOID
DcompReleaseResourceState(
    _Inout_ PDCOMP_RESOURCE Resource)
{
    if (Resource->Type == DCOMP_RESOURCE_SHARED_SECTION && Resource->Shared.Section)
        ObDereferenceObject(Resource->Shared.Section);
    RtlZeroMemory(Resource, sizeof(*Resource));
}

static
NTSTATUS
DcompCreateResource(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_CREATE* Command)
{
    PDCOMP_RESOURCE Resource;

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

    Resource = &Channel->Resources[Command->Resource - 1];
    if (Resource->Type)
        return STATUS_OBJECT_NAME_COLLISION;

    Resource->Type = Command->Type;
    return DcompEmit(Channel, DCOMP_MILCMD_CHANNEL_CREATE_RESOURCE, Command->Resource,
                     &Command->Type, sizeof(Command->Type), NULL, 0);
}

static
NTSTATUS
DcompSetFloat(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_SET_FLOAT* Command,
    _In_ PDCOMP_RESOURCE Resource)
{
    FLOAT Value = Command->Value;
    ULONG Payload[2];
    NTSTATUS Status = DcompUnbind(Channel, Command->Resource, Command->Property);

    if (!NT_SUCCESS(Status))
        return Status;

    if (DcompIsVisualType(Resource->Type))
    {
        switch (Command->Property)
        {
            case DCOMP_VISUAL_OFFSET_X:
            case DCOMP_VISUAL_OFFSET_Y:
            case DCOMP_VISUAL_OFFSET_Z:
                Resource->Visual.Offset[Command->Property - DCOMP_VISUAL_OFFSET_X] = Value;
                return DcompEmitOffset(Channel, Command->Resource, Resource);

            case DCOMP_VISUAL_WIDTH:
            case DCOMP_VISUAL_HEIGHT:
                Resource->Visual.Size[Command->Property - DCOMP_VISUAL_WIDTH] = Value;
                return DcompEmitSize(Channel, Command->Resource, Resource);

            case DCOMP_VISUAL_OPACITY:
                Value = min(max(Value, 0.0f), 1.0f);
                return DcompEmit(Channel, DCOMP_MILCMD_VISUAL_SET_OPACITY, Command->Resource,
                                 &Value, sizeof(Value), NULL, 0);
        }
    }

    Payload[0] = Command->Property;
    RtlCopyMemory(&Payload[1], &Value, sizeof(Value));
    return DcompEmit(Channel, DCOMP_MILCMD_RESOURCE_SET_FLOAT, Command->Resource, Payload, sizeof(Payload), NULL, 0);
}

static
NTSTATUS
DcompSetInteger(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_SET_INTEGER* Command,
    _In_ PDCOMP_RESOURCE Resource)
{
    ULONG Payload[4];
    NTSTATUS Status = DcompUnbind(Channel, Command->Resource, Command->Property);

    if (!NT_SUCCESS(Status))
        return Status;

    switch (Resource->Type)
    {
        case DCOMP_RESOURCE_ANIMATION:
            if (Command->Property == DCOMP_ANIMATION_SECTION_OFFSET)
            {
                Resource->Animation.Offset = (ULONG)Command->Value;
                return DcompEmitPrimitives(Channel, Command->Resource, Resource);
            }
            if (Command->Property == DCOMP_ANIMATION_SECTION_SIZE)
            {
                Resource->Animation.Size = (ULONG)Command->Value;
                return DcompEmitPrimitives(Channel, Command->Resource, Resource);
            }
            break;

        case DCOMP_RESOURCE_GDI_SPRITE_BITMAP:
            if (Command->Property == DCOMP_SPRITE_BITMAP_FORMAT)
            {
                Payload[0] = (ULONG)Command->Value;
                return DcompEmit(Channel, DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_FORMAT, Command->Resource,
                                 Payload, sizeof(ULONG), NULL, 0);
            }
            if (Command->Property == DCOMP_SPRITE_BITMAP_SURFACE)
                return DcompEmit(Channel, DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_SURFACE, Command->Resource,
                                 &Command->Value, sizeof(Command->Value), NULL, 0);
            break;

        case DCOMP_RESOURCE_WINDOW_NODE:
            if (Command->Property == DCOMP_WINDOW_NODE_WINDOW)
                return DcompEmit(Channel, DCOMP_MILCMD_WINDOW_NODE_SET_WINDOW_HANDLE, Command->Resource,
                                 &Command->Value, sizeof(Command->Value), NULL, 0);
            break;

        case DCOMP_RESOURCE_CACHED_VISUAL_IMAGE:
            if (Command->Property == DCOMP_CACHED_IMAGE_FREEZE)
                return DcompEmit(Channel, DCOMP_MILCMD_CACHED_VISUAL_IMAGE_FREEZE, Command->Resource, NULL, 0, NULL, 0);
            break;
    }

    if (DcompIsVisualType(Resource->Type) && Command->Property == DCOMP_VISUAL_VISIBLE)
    {
        Payload[0] = Command->Value ? TRUE : FALSE;
        return DcompEmit(Channel, DCOMP_MILCMD_VISUAL_SET_VISIBLE, Command->Resource, Payload, sizeof(ULONG), NULL, 0);
    }

    Payload[0] = Command->Property;
    Payload[1] = 0;
    RtlCopyMemory(&Payload[2], &Command->Value, sizeof(Command->Value));
    return DcompEmit(Channel, DCOMP_MILCMD_RESOURCE_SET_INTEGER, Command->Resource, Payload, sizeof(Payload), NULL, 0);
}

static
NTSTATUS
DcompSetHandle(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_SET_HANDLE* Command,
    _In_ PDCOMP_RESOURCE Resource)
{
    ULONG Payload[4];

    if (Resource->Type == DCOMP_RESOURCE_COMPOSITION_SURFACE_BITMAP &&
        Command->Property == DCOMP_COMPOSITION_SURFACE_HANDLE)
    {
        RtlZeroMemory(Payload, sizeof(Payload));
        RtlCopyMemory(Payload, &Command->Handle, sizeof(Command->Handle));
        return DcompEmit(Channel, DCOMP_MILCMD_COMPOSITION_SURFACE_UPDATE, Command->Resource,
                         Payload, sizeof(Payload), NULL, 0);
    }

    Payload[0] = Command->Property;
    Payload[1] = 0;
    RtlCopyMemory(&Payload[2], &Command->Handle, sizeof(Command->Handle));
    return DcompEmit(Channel, DCOMP_MILCMD_RESOURCE_SET_HANDLE, Command->Resource, Payload, sizeof(Payload), NULL, 0);
}

static
NTSTATUS
DcompSetReference(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_SET_REFERENCE* Command,
    _In_ PDCOMP_RESOURCE Resource)
{
    ULONG Payload[2];
    ULONG MilCommand = 0;

    if (Command->Value && !DcompLookupResource(Channel, Command->Value))
        return STATUS_INVALID_PARAMETER;

    if (DcompIsVisualType(Resource->Type))
    {
        switch (Command->Property)
        {
            case DCOMP_VISUAL_TRANSFORM:
                MilCommand = DCOMP_MILCMD_VISUAL_SET_TRANSFORM;
                break;
            case DCOMP_VISUAL_CLIP:
                MilCommand = DCOMP_MILCMD_VISUAL_SET_CLIP;
                break;
            case DCOMP_VISUAL_CONTENT:
                MilCommand = DCOMP_MILCMD_VISUAL_SET_CONTENT;
                break;
            case DCOMP_VISUAL_EFFECT:
                MilCommand = DCOMP_MILCMD_VISUAL_SET_EFFECT;
                break;
            case DCOMP_SPRITE_VISUAL_BRUSH:
                if (Resource->Type == DCOMP_RESOURCE_SPRITE_VISUAL)
                    MilCommand = DCOMP_MILCMD_SPRITE_VISUAL_SET_BRUSH;
                break;
            case DCOMP_WINDOW_NODE_SPRITE_BITMAP:
                if (Resource->Type == DCOMP_RESOURCE_WINDOW_NODE)
                    MilCommand = DCOMP_MILCMD_WINDOW_NODE_SET_SPRITE_BITMAP;
                break;
        }
    }
    else if (Resource->Type == DCOMP_RESOURCE_SURFACE_BRUSH && Command->Property == DCOMP_SURFACE_BRUSH_SURFACE)
    {
        MilCommand = DCOMP_MILCMD_SURFACE_BRUSH_SET_SURFACE;
    }
    else if (Resource->Type == DCOMP_RESOURCE_ANIMATION && Command->Property == DCOMP_ANIMATION_SECTION)
    {
        Resource->Animation.Section = Command->Value;
        return DcompEmitPrimitives(Channel, Command->Resource, Resource);
    }
    else if (Resource->Type == DCOMP_RESOURCE_CACHED_VISUAL_IMAGE && Command->Property == DCOMP_CACHED_IMAGE_VISUAL)
    {
        if (Command->Value && !DcompIsVisualType(DcompLookupResource(Channel, Command->Value)->Type))
            return STATUS_INVALID_PARAMETER;
        Resource->CachedImage.Visual = Command->Value;
        return DcompEmitCachedImage(Channel, Command->Resource, Resource);
    }

    if (MilCommand)
        return DcompEmit(Channel, MilCommand, Command->Resource, &Command->Value, sizeof(ULONG), NULL, 0);

    Payload[0] = Command->Property;
    Payload[1] = Command->Value;
    return DcompEmit(Channel, DCOMP_MILCMD_RESOURCE_SET_REFERENCE, Command->Resource, Payload, sizeof(Payload), NULL, 0);
}

static
NTSTATUS
DcompSetBuffer(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_SET_BUFFER* Command,
    _In_ PDCOMP_RESOURCE Resource)
{
    const VOID* Data = Command + 1;
    ULONG Payload[2];
    NTSTATUS Status = DcompUnbind(Channel, Command->Resource, Command->Property);

    if (!NT_SUCCESS(Status))
        return Status;

    if (DcompIsVisualType(Resource->Type))
    {
        if (Command->Property == DCOMP_VISUAL_OFFSET_VECTOR && Command->Size == sizeof(Resource->Visual.Offset))
        {
            RtlCopyMemory(Resource->Visual.Offset, Data, sizeof(Resource->Visual.Offset));
            return DcompEmitOffset(Channel, Command->Resource, Resource);
        }
        if (Command->Property == DCOMP_VISUAL_SIZE_VECTOR && Command->Size == sizeof(Resource->Visual.Size))
        {
            RtlCopyMemory(Resource->Visual.Size, Data, sizeof(Resource->Visual.Size));
            return DcompEmitSize(Channel, Command->Resource, Resource);
        }
    }

    switch (Resource->Type)
    {
        case DCOMP_RESOURCE_COLOR_BRUSH:
            if (Command->Property == DCOMP_COLOR_BRUSH_COLOR && Command->Size == 4 * sizeof(FLOAT))
                return DcompEmit(Channel, DCOMP_MILCMD_COLOR_BRUSH_SET_COLOR, Command->Resource,
                                 Data, Command->Size, NULL, 0);
            break;

        case DCOMP_RESOURCE_GDI_SPRITE_BITMAP:
            if (Command->Property == DCOMP_SPRITE_BITMAP_DIRTY_RECTS && !(Command->Size % sizeof(RECT)))
                return DcompEmit(Channel, DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_DIRTY, Command->Resource,
                                 Data, Command->Size, NULL, 0);
            if (Command->Property == DCOMP_SPRITE_BITMAP_MARGINS && Command->Size == 4 * sizeof(LONG))
                return DcompEmit(Channel, DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_MARGINS, Command->Resource,
                                 Data, Command->Size, NULL, 0);
            break;

        case DCOMP_RESOURCE_CACHED_VISUAL_IMAGE:
            if (Command->Property == DCOMP_CACHED_IMAGE_RECT && Command->Size == sizeof(Resource->CachedImage.Rect))
            {
                RtlCopyMemory(&Resource->CachedImage.Rect, Data, sizeof(Resource->CachedImage.Rect));
                return DcompEmitCachedImage(Channel, Command->Resource, Resource);
            }
            if (Command->Property == DCOMP_CACHED_IMAGE_SNAPSHOT && Command->Size == sizeof(RECT))
                return DcompEmit(Channel, DCOMP_MILCMD_CACHED_VISUAL_IMAGE_SNAPSHOT, Command->Resource,
                                 Data, Command->Size, NULL, 0);
            break;

        case DCOMP_RESOURCE_ANIMATION:
            if ((Command->Property == DCOMP_ANIMATION_BEGIN_TIME || Command->Property == DCOMP_ANIMATION_END_TIME) &&
                Command->Size == sizeof(LONGLONG))
            {
                if (Command->Property == DCOMP_ANIMATION_BEGIN_TIME)
                    RtlCopyMemory(&Resource->Animation.Begin, Data, sizeof(LONGLONG));
                else
                    RtlCopyMemory(&Resource->Animation.End, Data, sizeof(LONGLONG));
                return DcompEmitBeginTime(Channel, Command->Resource, Resource);
            }
            break;
    }

    Payload[0] = Command->Property;
    Payload[1] = Command->Size;
    return DcompEmit(Channel, DCOMP_MILCMD_RESOURCE_SET_BUFFER, Command->Resource,
                     Payload, sizeof(Payload), Data, Command->Size);
}

static
NTSTATUS
DcompSetReferenceArray(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_SET_REFERENCE_ARRAY* Command)
{
    const ULONG* Members = (const ULONG*)(Command + 1);
    ULONG Payload[2];
    ULONG Index;

    for (Index = 0; Index < Command->Count; Index++)
    {
        if (Members[Index] && !DcompLookupResource(Channel, Members[Index]))
            return STATUS_INVALID_PARAMETER;
    }

    Payload[0] = Command->Property;
    Payload[1] = Command->Count;
    return DcompEmit(Channel, DCOMP_MILCMD_RESOURCE_SET_REFERENCE_ARRAY, Command->Resource,
                     Payload, sizeof(Payload), Members, Command->Count * sizeof(ULONG));
}

static
NTSTATUS
DcompAddChild(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_ADD_CHILD* Command)
{
    PDCOMP_RESOURCE Parent = DcompLookupResource(Channel, Command->Parent);
    PDCOMP_RESOURCE Child = DcompLookupResource(Channel, Command->Child);
    PDCOMP_RESOURCE Reference = Command->Reference ? DcompLookupResource(Channel, Command->Reference) : NULL;
    ULONG Payload[3];

    if (!Parent || !Child || !DcompIsVisualType(Parent->Type) || !DcompIsVisualType(Child->Type) ||
        Command->Parent == Command->Child || (Command->Reference && (!Reference || !DcompIsVisualType(Reference->Type))))
        return STATUS_INVALID_PARAMETER;

    Payload[0] = Command->Child;
    Payload[1] = Command->Reference;
    Payload[2] = Command->InsertAbove ? TRUE : FALSE;
    return DcompEmit(Channel, DCOMP_MILCMD_VISUAL_INSERT_CHILD, Command->Parent, Payload, sizeof(Payload), NULL, 0);
}

static
NTSTATUS
DcompRemoveChild(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ const DCOMP_CMD_REMOVE_CHILD* Command)
{
    PDCOMP_RESOURCE Parent = DcompLookupResource(Channel, Command->Parent);

    if (!Parent || !DcompIsVisualType(Parent->Type))
        return STATUS_INVALID_PARAMETER;
    if (!Command->Child)
        return DcompEmit(Channel, DCOMP_MILCMD_VISUAL_REMOVE_ALL_CHILDREN, Command->Parent, NULL, 0, NULL, 0);
    if (!DcompLookupResource(Channel, Command->Child))
        return STATUS_INVALID_PARAMETER;
    return DcompEmit(Channel, DCOMP_MILCMD_VISUAL_REMOVE_CHILD, Command->Parent,
                     &Command->Child, sizeof(ULONG), NULL, 0);
}

static
NTSTATUS
DcompProcessBuffer(
    _In_ PDCOMP_CHANNEL Channel,
    _In_reads_bytes_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN TopLevel,
    _Inout_ PULONG Count);

/* The size of the command at Record, or 0 when it does not fit in Remaining */
static
ULONG
DcompCommandSize(
    _In_reads_bytes_(Remaining) const UCHAR* Record,
    _In_ ULONG Remaining)
{
    ULONG Tag = *(const ULONG*)Record;
    ULONG Size;

    switch (Tag)
    {
        case DCOMP_CMD_OUT_OF_LINE:
            Size = sizeof(DCOMP_CMD_OUT_OF_LINE_BUFFER);
            break;
        case DCOMP_CMD_ACTIVATE_CHANNEL_TRIGGER:
        case DCOMP_CMD_RELEASE_RESOURCE:
            Size = 2 * sizeof(ULONG);
            break;
        case DCOMP_CMD_CREATE_RESOURCE:
            Size = sizeof(DCOMP_CMD_CREATE);
            break;
        case DCOMP_CMD_OPEN_SHARED_RESOURCE:
            Size = sizeof(DCOMP_CMD_OPEN_SHARED);
            break;
        case DCOMP_CMD_SET_CHANNEL_INTEGER_PROPERTY:
            Size = sizeof(DCOMP_CMD_SET_CHANNEL_INTEGER);
            break;
        case DCOMP_CMD_GET_ANIMATION_TIME:
        case DCOMP_CMD_CAPTURE_POINTER:
            Size = sizeof(DCOMP_CMD_ANIMATION_TIME);
            break;
        case DCOMP_CMD_ACTIVATE_RESOURCE_TRIGGER:
        case DCOMP_CMD_SET_RESOURCE_CALLBACK_ID:
            Size = sizeof(DCOMP_CMD_RESOURCE_VALUE);
            break;
        case DCOMP_CMD_OPEN_SHARED_RESOURCE_HANDLE:
            Size = sizeof(DCOMP_CMD_OPEN_SHARED_HANDLE);
            break;
        case DCOMP_CMD_SET_INTEGER_PROPERTY:
            Size = sizeof(DCOMP_CMD_SET_INTEGER);
            break;
        case DCOMP_CMD_SET_FLOAT_PROPERTY:
            Size = sizeof(DCOMP_CMD_SET_FLOAT);
            break;
        case DCOMP_CMD_SET_HANDLE_PROPERTY:
            Size = sizeof(DCOMP_CMD_SET_HANDLE);
            break;
        case DCOMP_CMD_SET_REFERENCE_PROPERTY:
            Size = sizeof(DCOMP_CMD_SET_REFERENCE);
            break;
        case DCOMP_CMD_SET_ANIMATION_PROPERTY:
            Size = sizeof(DCOMP_CMD_SET_ANIMATION);
            break;
        case DCOMP_CMD_SET_DELETED_NOTIFICATION_TAG:
            Size = sizeof(DCOMP_CMD_SET_DELETED_TAG);
            break;
        case DCOMP_CMD_ADD_VISUAL_CHILD:
            Size = sizeof(DCOMP_CMD_ADD_CHILD);
            break;
        case DCOMP_CMD_REDIRECT_MOUSE_TO_HWND:
            Size = DCOMP_REDIRECT_MOUSE_COMMAND_SIZE;
            break;
        case DCOMP_CMD_SET_VISUAL_INPUT_SINK:
            Size = sizeof(DCOMP_CMD_SET_INPUT_SINK);
            break;
        case DCOMP_CMD_REMOVE_VISUAL_CHILD:
            Size = sizeof(DCOMP_CMD_REMOVE_CHILD);
            break;

        case DCOMP_CMD_SET_HANDLE_ARRAY_PROPERTY:
        case DCOMP_CMD_SET_BUFFER_PROPERTY:
        case DCOMP_CMD_SET_REFERENCE_ARRAY_PROPERTY:
        {
            /* The three variable commands share the header layout; the fourth dword sizes the tail */
            const DCOMP_CMD_SET_BUFFER* Header = (const DCOMP_CMD_SET_BUFFER*)Record;
            ULONG Element = (Tag == DCOMP_CMD_SET_BUFFER_PROPERTY) ? 1 :
                            (Tag == DCOMP_CMD_SET_HANDLE_ARRAY_PROPERTY) ? sizeof(ULONG64) : sizeof(ULONG);
            if (Remaining < sizeof(*Header) || Header->Size > (Remaining - sizeof(*Header)) / Element)
                return 0;
            Size = sizeof(*Header) + ALIGN_UP_BY(Header->Size * Element, sizeof(ULONG));
            break;
        }

        default:
            return 0;
    }

    return (Size <= Remaining) ? Size : 0;
}

/* Validates one client command and turns it into compositor commands */
static
NTSTATUS
DcompProcessCommand(
    _In_ PDCOMP_CHANNEL Channel,
    _In_reads_bytes_(Size) const UCHAR* Record,
    _In_ ULONG Size,
    _In_ BOOLEAN TopLevel,
    _Inout_ PULONG Count)
{
    ULONG Tag = *(const ULONG*)Record;
    PDCOMP_RESOURCE Resource = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    /* Every property command starts { tag, resource, property } */
    if (Tag >= DCOMP_CMD_SET_INTEGER_PROPERTY && Tag <= DCOMP_CMD_SET_REFERENCE_ARRAY_PROPERTY)
    {
        Resource = DcompLookupResource(Channel, ((const ULONG*)Record)[1]);
        if (!Resource)
            return STATUS_INVALID_HANDLE;
    }

    switch (Tag)
    {
        case DCOMP_CMD_OUT_OF_LINE:
        {
            const DCOMP_CMD_OUT_OF_LINE_BUFFER* Command = (const DCOMP_CMD_OUT_OF_LINE_BUFFER*)Record;
            PUCHAR Copy;

            if (!TopLevel || !Command->Size || Command->Size > 16 * DCOMP_CHANNEL_BUFFER_SIZE)
                return STATUS_INVALID_PARAMETER;
            Copy = ExAllocatePoolWithTag(PagedPool, Command->Size, TAG_DCOMP);
            if (!Copy)
                return STATUS_NO_MEMORY;
            _SEH2_TRY
            {
                ProbeForRead((PVOID)(ULONG_PTR)Command->Buffer, Command->Size, sizeof(ULONG));
                RtlCopyMemory(Copy, (PVOID)(ULONG_PTR)Command->Buffer, Command->Size);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            if (NT_SUCCESS(Status))
                Status = DcompProcessBuffer(Channel, Copy, Command->Size, FALSE, Count);
            ExFreePoolWithTag(Copy, TAG_DCOMP);
            return Status;
        }

        case DCOMP_CMD_CREATE_RESOURCE:
            Status = DcompCreateResource(Channel, (const DCOMP_CMD_CREATE*)Record);
            break;

        case DCOMP_CMD_RELEASE_RESOURCE:
        {
            ULONG Handle = ((const DCOMP_CMD_RELEASE*)Record)->Resource;
            Resource = DcompLookupResource(Channel, Handle);
            if (!Resource)
                return STATUS_INVALID_HANDLE;
            Status = DcompEmit(Channel, DCOMP_MILCMD_CHANNEL_RELEASE_RESOURCE, Handle, NULL, 0, NULL, 0);
            DcompForgetBindings(Channel, Handle);
            DcompReleaseResourceState(Resource);
            break;
        }

        case DCOMP_CMD_SET_INTEGER_PROPERTY:
            Status = DcompSetInteger(Channel, (const DCOMP_CMD_SET_INTEGER*)Record, Resource);
            break;

        case DCOMP_CMD_SET_FLOAT_PROPERTY:
            Status = DcompSetFloat(Channel, (const DCOMP_CMD_SET_FLOAT*)Record, Resource);
            break;

        case DCOMP_CMD_SET_HANDLE_PROPERTY:
            Status = DcompSetHandle(Channel, (const DCOMP_CMD_SET_HANDLE*)Record, Resource);
            break;

        case DCOMP_CMD_SET_BUFFER_PROPERTY:
            Status = DcompSetBuffer(Channel, (const DCOMP_CMD_SET_BUFFER*)Record, Resource);
            break;

        case DCOMP_CMD_SET_REFERENCE_PROPERTY:
            Status = DcompSetReference(Channel, (const DCOMP_CMD_SET_REFERENCE*)Record, Resource);
            break;

        case DCOMP_CMD_SET_REFERENCE_ARRAY_PROPERTY:
            Status = DcompSetReferenceArray(Channel, (const DCOMP_CMD_SET_REFERENCE_ARRAY*)Record);
            break;

        case DCOMP_CMD_SET_ANIMATION_PROPERTY:
            Status = DcompBind(Channel, (const DCOMP_CMD_SET_ANIMATION*)Record);
            break;

        case DCOMP_CMD_ADD_VISUAL_CHILD:
            Status = DcompAddChild(Channel, (const DCOMP_CMD_ADD_CHILD*)Record);
            break;

        case DCOMP_CMD_REMOVE_VISUAL_CHILD:
            Status = DcompRemoveChild(Channel, (const DCOMP_CMD_REMOVE_CHILD*)Record);
            break;

        /* Accepted so Windows clients carry on; nothing acts on these yet */
        case DCOMP_CMD_ACTIVATE_CHANNEL_TRIGGER:
        case DCOMP_CMD_SET_CHANNEL_INTEGER_PROPERTY:
        case DCOMP_CMD_ACTIVATE_RESOURCE_TRIGGER:
        case DCOMP_CMD_SET_RESOURCE_CALLBACK_ID:
        case DCOMP_CMD_SET_DELETED_NOTIFICATION_TAG:
        case DCOMP_CMD_SET_VISUAL_INPUT_SINK:
            break;

        /* Shared resources, animation time queries, pointer capture and handle arrays come later */
        default:
            Status = STATUS_NOT_SUPPORTED;
            break;
    }

    if (NT_SUCCESS(Status))
        (*Count)++;
    return Status;
}

static
NTSTATUS
DcompProcessBuffer(
    _In_ PDCOMP_CHANNEL Channel,
    _In_reads_bytes_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN TopLevel,
    _Inout_ PULONG Count)
{
    ULONG Offset = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    while (Length - Offset >= sizeof(ULONG) && NT_SUCCESS(Status))
    {
        ULONG Size = DcompCommandSize(Buffer + Offset, Length - Offset);
        if (!Size)
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
        Status = DcompProcessCommand(Channel, Buffer + Offset, Size, TopLevel, Count);
        if (!NT_SUCCESS(Status))
            ERR("DComp channel %lu: command %lu at offset %lu failed 0x%08lx\n",
                Channel->Id, *(const ULONG*)(Buffer + Offset), Offset, Status);
        Offset += Size;
    }
    return Status;
}

static
VOID
DcompDestroyChannel(
    _In_ PDCOMP_CHANNEL Channel,
    _In_ BOOLEAN FreeUserBuffer)
{
    SIZE_T Size = 0;
    ULONG Index;

    RemoveEntryList(&Channel->Link);
    if (gDcompCompositor)
        DcompQueueNode(DCOMP_BATCH_CLOSE_CHANNEL, Channel->Id, 0, NULL, 0);

    if (FreeUserBuffer && Channel->UserBuffer)
        ZwFreeVirtualMemory(NtCurrentProcess(), &Channel->UserBuffer, &Size, MEM_RELEASE);
    if (Channel->Resources)
    {
        for (Index = 0; Index < Channel->ResourceCount; Index++)
            DcompReleaseResourceState(&Channel->Resources[Index]);
        ExFreePoolWithTag(Channel->Resources, TAG_DCOMP);
    }
    if (Channel->Bindings)
        ExFreePoolWithTag(Channel->Bindings, TAG_DCOMP);
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

    Channel->Id = DcompAllocateChannelId();
    if (!Channel->Id)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    Size = ALIGN_UP_BY(min(max(Requested, PAGE_SIZE), DCOMP_CHANNEL_BUFFER_SIZE), PAGE_SIZE);
    Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &Channel->UserBuffer, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Channel->BufferSize = Size;
    Channel->Process = PsGetCurrentProcess();

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
    ULONG Count = 0;
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

        Status = DcompProcessBuffer(Channel, Copy, Length, TRUE, &Count);
    }

    /* The count is what was processed, which Windows' dcomp checks against what it wrote */
    _SEH2_TRY
    {
        ProbeForWrite(Consumed, sizeof(*Consumed), sizeof(ULONG));
        ProbeForWrite(MoreWork, sizeof(*MoreWork), sizeof(BOOLEAN));
        *Consumed = Count;
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

/*
 * Gives a shared section resource its section: the caller gets a handle to map and write it,
 * the compositor its own handle to read it.
 */
NTSTATUS
APIENTRY
NtDCompositionCreateAndBindSharedSection(
    _In_ HANDLE ChannelHandle,
    _In_ ULONG ResourceHandle,
    _In_ ULONG Size,
    _Out_ PHANDLE SectionHandle)
{
    PDCOMP_CHANNEL Channel;
    PDCOMP_RESOURCE Resource;
    OBJECT_ATTRIBUTES Attributes;
    LARGE_INTEGER MaximumSize;
    HANDLE KernelHandle = NULL;
    HANDLE ClientHandle = NULL;
    HANDLE CompositorHandle = NULL;
    PVOID Section = NULL;
    KAPC_STATE ApcState;
    ULONG64 Payload[2];
    NTSTATUS Status;

    UserEnterExclusive();

    Channel = DcompLookupChannel(ChannelHandle);
    Resource = Channel ? DcompLookupResource(Channel, ResourceHandle) : NULL;
    if (!Resource || Resource->Type != DCOMP_RESOURCE_SHARED_SECTION || Resource->Shared.Section)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }
    if (!Size || Size > DCOMP_MAX_SHARED_SECTION || !gDcompCompositor)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    MaximumSize.QuadPart = ALIGN_UP_BY(Size, PAGE_SIZE);
    Status = ZwCreateSection(&KernelHandle, SECTION_ALL_ACCESS, &Attributes, &MaximumSize,
                             PAGE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(Status))
        goto Exit;
    Status = ObReferenceObjectByHandle(KernelHandle, SECTION_ALL_ACCESS, NULL, KernelMode, &Section, NULL);
    ZwClose(KernelHandle);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Status = ObOpenObjectByPointer(Section, 0, NULL, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
                                   MmSectionObjectType, UserMode, &ClientHandle);
    if (!NT_SUCCESS(Status))
        goto Exit;

    KeStackAttachProcess((PKPROCESS)gDcompCompositor, &ApcState);
    Status = ObOpenObjectByPointer(Section, 0, NULL, SECTION_MAP_READ | SECTION_QUERY,
                                   MmSectionObjectType, UserMode, &CompositorHandle);
    KeUnstackDetachProcess(&ApcState);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Payload[0] = (ULONG64)(ULONG_PTR)CompositorHandle;
    Payload[1] = Size;
    Status = DcompEmit(Channel, DCOMP_MILCMD_SHARED_SECTION_UPDATE, ResourceHandle, Payload,
                       sizeof(ULONG64) + sizeof(ULONG), NULL, 0);
    if (!NT_SUCCESS(Status))
        goto Exit;
    CompositorHandle = NULL;

    _SEH2_TRY
    {
        ProbeForWrite(SectionHandle, sizeof(*SectionHandle), sizeof(ULONG));
        *SectionHandle = ClientHandle;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        goto Exit;

    Resource->Shared.Section = Section;
    Resource->Shared.Size = Size;
    Section = NULL;
    ClientHandle = NULL;

Exit:
    if (ClientHandle)
        ZwClose(ClientHandle);
    if (CompositorHandle)
    {
        KeStackAttachProcess((PKPROCESS)gDcompCompositor, &ApcState);
        ZwClose(CompositorHandle);
        KeUnstackDetachProcess(&ApcState);
    }
    if (Section)
        ObDereferenceObject(Section);
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
        Status = DcompQueueNode(DCOMP_BATCH_RESOURCE_COMMANDS, Channel->Id, Channel->PendingSize,
                                Channel->Pending, Channel->PendingSize);
        if (!NT_SUCCESS(Status))
            goto Exit;
        Channel->PendingSize = 0;
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
        Total += sizeof(DCOMP_BATCH_NODE) + CONTAINING_RECORD(Entry, DCOMP_QUEUED, Link)->Size;

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
            PDCOMP_BATCH_NODE Out = (PDCOMP_BATCH_NODE)Cursor;

            RtlZeroMemory(Out, sizeof(*Out));
            Out->Type = Node->Type;
            Out->Channel = Node->Channel;
            Out->Value = Node->Value;
            Cursor += sizeof(*Out);

            if (Node->Size)
            {
                Out->Commands = (ULONG64)(ULONG_PTR)Cursor;
                RtlCopyMemory(Cursor, Node->Data, Node->Size);
                Cursor += Node->Size;
            }

            if (Previous)
                Previous->Next = (ULONG64)(ULONG_PTR)Out;
            Previous = Out;
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
