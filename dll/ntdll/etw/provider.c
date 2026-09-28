/*
 * PROJECT:     ReactOS ntdll
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Providers writing events, put out on the debug output
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Nothing here keeps a trace session, so an event has nowhere to be collected.
 * What it does have is somewhere to be read: a module built for Windows says
 * what it is doing through these calls and through nothing else, so the event
 * is written to the debug output with the provider that wrote it. That is the
 * difference between a module that fails silently and one that says why.
 */

#include <ntdll.h>

#include <wmistr.h>
#include <evntrace.h>
#include <evntprov.h>

#define NDEBUG
#include <debug.h>

/* How many providers one process may have registered at once */
#define ETW_MAX_PROVIDERS 64

/*
 * How much of one field is shown before the rest is left out. A module
 * reporting a failure puts the whole of what went wrong in one field, so
 * this is as much as one debug line will carry rather than a sample.
 */
#define ETW_FIELD_SHOWN 220

typedef struct _ETW_PROVIDER
{
    GUID Id;
    PENABLECALLBACK Callback;
    PVOID Context;
    BOOLEAN InUse;
} ETW_PROVIDER, *PETW_PROVIDER;

static ETW_PROVIDER EtwProviders[ETW_MAX_PROVIDERS];
static RTL_CRITICAL_SECTION EtwProviderLock;
static BOOLEAN EtwProviderLockReady = FALSE;

/* The lock is made on the first registration, since there is no init to hook */
static
VOID
EtwpLockProviders(VOID)
{
    if (!EtwProviderLockReady)
    {
        RtlInitializeCriticalSection(&EtwProviderLock);
        EtwProviderLockReady = TRUE;
    }

    RtlEnterCriticalSection(&EtwProviderLock);
}

static
VOID
EtwpUnlockProviders(VOID)
{
    RtlLeaveCriticalSection(&EtwProviderLock);
}

/* A registration handle is the slot it sits in, one based so zero stays free */
static
PETW_PROVIDER
EtwpProviderFromHandle(
    _In_ REGHANDLE Handle)
{
    ULONG64 Slot = Handle;

    if ((Slot == 0) || (Slot > ETW_MAX_PROVIDERS))
        return NULL;

    if (!EtwProviders[Slot - 1].InUse)
        return NULL;

    return &EtwProviders[Slot - 1];
}

/**
 * @brief
 * Takes a provider's identity so its events can be named when they are written.
 */
ULONG
NTAPI
EtwEventRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PVOID EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    ULONG Index;

    if ((ProviderId == NULL) || (RegHandle == NULL))
        return ERROR_INVALID_PARAMETER;

    *RegHandle = 0;

    EtwpLockProviders();

    for (Index = 0; Index < ETW_MAX_PROVIDERS; Index++)
    {
        if (EtwProviders[Index].InUse)
            continue;

        EtwProviders[Index].Id = *ProviderId;
        EtwProviders[Index].Callback = (PENABLECALLBACK)EnableCallback;
        EtwProviders[Index].Context = CallbackContext;
        EtwProviders[Index].InUse = TRUE;
        *RegHandle = Index + 1;
        break;
    }

    EtwpUnlockProviders();

    if (*RegHandle == 0)
        return ERROR_NOT_ENOUGH_MEMORY;

    DPRINT1("Etw: provider {%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x} registered\n",
            ProviderId->Data1, ProviderId->Data2, ProviderId->Data3,
            ProviderId->Data4[0], ProviderId->Data4[1], ProviderId->Data4[2],
            ProviderId->Data4[3], ProviderId->Data4[4], ProviderId->Data4[5],
            ProviderId->Data4[6], ProviderId->Data4[7]);

    /*
     * A provider built on trace logging keeps its own idea of whether anything
     * is listening, and it only writes when its callback has said so. Answering
     * TRUE from EtwEventEnabled is not enough: that provider never asks. So the
     * callback is made here, at every level and keyword, and the provider then
     * writes what it has to say. This is the same call a session enabling the
     * provider would cause, and it is made outside the lock because a callback
     * is free to come back in and register more.
     */
    if (EnableCallback != NULL)
    {
        ((PENABLECALLBACK)EnableCallback)(ProviderId,
                                         EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                         TRACE_LEVEL_VERBOSE,
                                         ~((ULONGLONG)0),
                                         0,
                                         NULL,
                                         CallbackContext);
    }

    return ERROR_SUCCESS;
}

/**
 * @brief
 * Gives a provider's slot back.
 */
ULONG
NTAPI
EtwEventUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETW_PROVIDER Provider;

    EtwpLockProviders();

    Provider = EtwpProviderFromHandle(RegHandle);
    if (Provider != NULL)
        Provider->InUse = FALSE;

    EtwpUnlockProviders();

    return ERROR_SUCCESS;
}

/**
 * @brief
 * Says whether anything would collect an event of this description.
 *
 * @return
 * TRUE, so that a provider goes on to write the event and it can be read here.
 * A provider told otherwise says nothing at all, which is the opposite of what
 * this is for.
 */
BOOLEAN
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);

    return TRUE;
}

/**
 * @brief
 * Says whether a provider would be collected at this level and keyword.
 */
BOOLEAN
NTAPI
EtwEventProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);

    return TRUE;
}

/*
 * The event's description carries the name of every field after its own, each
 * one followed by the bytes that say how to read it. This hands back one name
 * per call and steps over the rest.
 */
static
PCSTR
EtwpNextFieldName(
    _Inout_ PUCHAR *Walk,
    _In_ PUCHAR End)
{
    PUCHAR Name = *Walk;
    PUCHAR Scan = Name;
    UCHAR Type;

    while ((Scan < End) && (*Scan != 0))
        Scan++;

    if (Scan >= End)
        return NULL;

    /* Past the terminator, the type of the field */
    Scan++;
    if (Scan >= End)
        return NULL;

    Type = *Scan++;

    /* A count of its own follows a field that holds more than one of something */
    if ((Type & 0x40) != 0)
        Scan += sizeof(USHORT);

    /* And how it should be shown follows the type, with its own tags after that */
    if ((Type & 0x80) != 0)
    {
        UCHAR Shown = (Scan < End) ? *Scan++ : 0;

        if ((Shown & 0x80) != 0)
        {
            while ((Scan < End) && ((*Scan & 0x80) != 0))
                Scan++;

            if (Scan < End)
                Scan++;
        }
    }

    if (Scan > End)
        return NULL;

    *Walk = Scan;
    return (PCSTR)Name;
}

/* One field of an event, shown as text when it reads as text and as bytes when not */
static
VOID
EtwpPrintData(
    _In_ ULONG Index,
    _In_opt_ PCSTR FieldName,
    _In_ PEVENT_DATA_DESCRIPTOR Data)
{
    PUCHAR Bytes = (PUCHAR)(ULONG_PTR)Data->Ptr;
    ULONG Size = Data->Size;
    CHAR Label[72];
    ULONG Shown;
    ULONG Walk;
    ULONG Part;
    BOOLEAN Text;

    if (FieldName != NULL)
        _snprintf(Label, sizeof(Label) - 1, "[%lu] %s =", Index, FieldName);
    else
        _snprintf(Label, sizeof(Label) - 1, "[%lu]", Index);

    Label[sizeof(Label) - 1] = ANSI_NULL;

    if ((Bytes == NULL) || (Size == 0))
    {
        DPRINT1("Etw:   %s empty\n", Label);
        return;
    }

    /*
     * Some fields are counted rather than terminated: the field opens with its
     * own length and the text follows. Taking that length for text is what puts
     * an unreadable pair of bytes in front of every name.
     */
    if ((Size > sizeof(USHORT)) && (*(PUSHORT)Bytes == (USHORT)Size))
    {
        Bytes += sizeof(USHORT);
        Size -= sizeof(USHORT);
    }

    /*
     * The second field of an event is not data at all, it is the event's own
     * description: tag bytes, then the name, then the names of the fields. The
     * name is the one part worth reading, and without it every event is a number.
     */
    if (Index == 1)
    {
        Walk = 0;
        while ((Walk < Size) && ((Bytes[Walk] & 0x80) != 0))
            Walk++;

        Walk++;

        if (Walk < Size)
        {
            DPRINT1("Etw:   %s event '%.*s'\n", Label,
                    (int)min(Size - Walk, ETW_FIELD_SHOWN), Bytes + Walk);
            return;
        }
    }

    /*
     * A field holding text carries its terminator, so the text ends at the
     * first zero rather than at the end of the field. Reading to the end
     * instead would find that zero and take the whole field for bytes, which
     * is how the one field worth reading comes out as four hex numbers.
     */
    Shown = 0;
    while ((Shown < Size) && (Bytes[Shown] != 0))
        Shown++;

    /*
     * Wide text is the common case, so it is worth reading it out as text. Four
     * bytes are left out of it: a small number has a zero for its upper bytes
     * and reads as a pair of characters, and numbers that size are everywhere.
     */
    if ((Size > 4) && ((Size % 2) == 0) && (Bytes[1] == 0) && (Bytes[3] == 0))
    {
        Walk = 0;
        Shown = Size / sizeof(WCHAR);

        while (Shown > 0)
        {
            Part = min(Shown, ETW_FIELD_SHOWN);

            if (Walk == 0)
                DPRINT1("Etw:   %s %u bytes, '%.*S'\n", Label, Size, (int)Part,
                        (PWCHAR)Bytes);
            else
                DPRINT1("Etw:        '%.*S'\n", (int)Part, (PWCHAR)Bytes + Walk);

            Walk += Part;
            Shown -= Part;
        }

        return;
    }

    Text = (Shown > 1);
    for (Walk = 0; Walk < Shown; Walk++)
    {
        if ((Bytes[Walk] == '\r') || (Bytes[Walk] == '\n') || (Bytes[Walk] == '\t'))
            continue;

        if ((Bytes[Walk] < 0x20) || (Bytes[Walk] > 0x7E))
        {
            Text = FALSE;
            break;
        }
    }

    if (Text)
    {
        Walk = 0;

        while (Shown > 0)
        {
            Part = min(Shown, ETW_FIELD_SHOWN);

            if (Walk == 0)
                DPRINT1("Etw:   %s %u bytes, '%.*s'\n", Label, Size, (int)Part, Bytes);
            else
                DPRINT1("Etw:        '%.*s'\n", (int)Part, Bytes + Walk);

            Walk += Part;
            Shown -= Part;
        }

        return;
    }

    switch (Size)
    {
        case 1:
            DPRINT1("Etw:   %s %02x\n", Label, Bytes[0]);
            break;
        case 2:
            DPRINT1("Etw:   %s %04x\n", Label, *(PUSHORT)Bytes);
            break;
        case 4:
            DPRINT1("Etw:   %s %08lx\n", Label, *(PULONG)Bytes);
            break;
        case 8:
            DPRINT1("Etw:   %s %I64x\n", Label, *(PULONG64)Bytes);
            break;
        default:
            DPRINT1("Etw:   %s %u bytes, %02x %02x %02x %02x\n", Label, Size,
                    Bytes[0], (Size > 1) ? Bytes[1] : 0,
                    (Size > 2) ? Bytes[2] : 0, (Size > 3) ? Bytes[3] : 0);
            break;
    }
}

/**
 * @brief
 * Writes one event out where it can be read.
 */
ULONG
NTAPI
EtwEventWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG UserDataCount,
    _In_opt_ PEVENT_DATA_DESCRIPTOR UserData)
{
    PETW_PROVIDER Provider;
    GUID Id = { 0 };
    PUCHAR Meta = NULL;
    PUCHAR MetaEnd = NULL;
    ULONG Index;

    if (EventDescriptor == NULL)
        return ERROR_INVALID_PARAMETER;

    EtwpLockProviders();
    Provider = EtwpProviderFromHandle(RegHandle);
    if (Provider != NULL)
        Id = Provider->Id;
    EtwpUnlockProviders();

    DPRINT1("Etw: {%08lx} event %u level %u keyword %I64x, %lu fields\n",
            Id.Data1, EventDescriptor->Id, EventDescriptor->Level,
            EventDescriptor->Keyword, UserDataCount);

    if (UserData == NULL)
        return ERROR_SUCCESS;

    /*
     * Walk past the event's own size, tags and name, so what is left is the
     * names of the fields that follow, in the order they are written.
     */
    if ((UserDataCount >= 2) &&
        (UserData[1].Ptr != 0) &&
        (UserData[1].Size > sizeof(USHORT)))
    {
        Meta = (PUCHAR)(ULONG_PTR)UserData[1].Ptr;
        MetaEnd = Meta + UserData[1].Size;
        Meta += sizeof(USHORT);

        while ((Meta < MetaEnd) && ((*Meta & 0x80) != 0))
            Meta++;

        if (Meta < MetaEnd)
            Meta++;

        while ((Meta < MetaEnd) && (*Meta != 0))
            Meta++;

        if (Meta < MetaEnd)
            Meta++;
    }

    for (Index = 0; Index < UserDataCount; Index++)
    {
        PCSTR FieldName = NULL;

        /* The first two fields describe the event rather than belong to it */
        if ((Index >= 2) && (Meta != NULL) && (Meta < MetaEnd))
            FieldName = EtwpNextFieldName(&Meta, MetaEnd);

        EtwpPrintData(Index, FieldName, &UserData[Index]);
    }

    return ERROR_SUCCESS;
}

/**
 * @brief
 * Writes one event that names the activity it belongs to.
 */
ULONG
NTAPI
EtwEventWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_opt_ PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);

    return EtwEventWrite(RegHandle, EventDescriptor, UserDataCount, UserData);
}

/**
 * @brief
 * Writes one event, with more said about how it is to be written.
 */
ULONG
NTAPI
EtwEventWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_opt_ PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);

    return EtwEventWrite(RegHandle, EventDescriptor, UserDataCount, UserData);
}

/**
 * @brief
 * Writes one event whose whole content is a string.
 */
ULONG
NTAPI
EtwEventWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ PCWSTR String)
{
    PETW_PROVIDER Provider;
    GUID Id = { 0 };

    EtwpLockProviders();
    Provider = EtwpProviderFromHandle(RegHandle);
    if (Provider != NULL)
        Id = Provider->Id;
    EtwpUnlockProviders();

    DPRINT1("Etw: {%08lx} level %u keyword %I64x, '%S'\n",
            Id.Data1, Level, Keyword, (String != NULL) ? String : L"");

    return ERROR_SUCCESS;
}

/**
 * @brief
 * Takes something a provider wants remembered about itself.
 *
 * @remarks
 * What is set is the provider's traits and its group, which only matter to a
 * session deciding what to collect. Nothing collects here, so it is taken and
 * the provider is told it was kept.
 */
ULONG
NTAPI
EtwEventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_ PVOID Information,
    _In_ ULONG Length)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(InformationClass);
    UNREFERENCED_PARAMETER(Information);
    UNREFERENCED_PARAMETER(Length);

    return ERROR_SUCCESS;
}

/**
 * @brief
 * Keeps or reads the activity a thread's events belong to.
 */
ULONG
NTAPI
EtwEventActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    static ULONG EtwActivityCount = 0;

    if (ActivityId == NULL)
        return ERROR_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_CREATE_ID:
        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            RtlZeroMemory(ActivityId, sizeof(*ActivityId));
            ActivityId->Data1 = ++EtwActivityCount;
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_GET_ID:
            RtlZeroMemory(ActivityId, sizeof(*ActivityId));
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            return ERROR_SUCCESS;

        default:
            return ERROR_INVALID_PARAMETER;
    }
}

/* EOF */
