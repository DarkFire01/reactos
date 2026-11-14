/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel mode manifest based event providers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>
#include <evntprov.h>

#define NDEBUG
#include <debug.h>

/*
 * There are no trace sessions. A built-in debug session enables every provider
 * up to the level in Control\WMI\EtwDebugPrint and prints what they write to
 * the debug log, decoding TraceLogging events from their own metadata.
 */

#define TAG_ETW_PROVIDER            'PwtE'
#define ETWP_PROVIDER_SIGNATURE     0x50777445
#define ETWP_NAME_LENGTH            48

/* Low byte of EVENT_DATA_DESCRIPTOR.Reserved */
#define ETWP_DATA_EVENT_METADATA    1
#define ETWP_DATA_PROVIDER_METADATA 2

/* TraceLogging field type bits */
#define ETWP_IN_TYPE_MASK           0x1F
#define ETWP_IN_VCOUNT              0x20
#define ETWP_IN_CCOUNT              0x40
#define ETWP_IN_CHAIN               0x80

typedef struct _ETWP_KERNEL_PROVIDER
{
    ULONG Signature;
    GUID ProviderId;
    PETWENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
    UCHAR Level;
    CHAR Name[ETWP_NAME_LENGTH];
} ETWP_KERNEL_PROVIDER, *PETWP_KERNEL_PROVIDER;

static UCHAR EtwpDebugLevel;
static BOOLEAN EtwpDebugLevelRead;

/* Callable at any IRQL, so this only looks at the nonpaged registration */
static
BOOLEAN
EtwpValidProvider(
    _In_ REGHANDLE RegHandle)
{
    PETWP_KERNEL_PROVIDER Provider = (PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle;

    return Provider && Provider->Signature == ETWP_PROVIDER_SIGNATURE;
}

static
VOID
EtwpReadDebugLevel(VOID)
{
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\WMI\\EtwDebugPrint");
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"Level");
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    ULONG Length;

    PAGED_CODE();

    if (EtwpDebugLevelRead)
        return;

    InitializeObjectAttributes(&Attributes, &KeyName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Attributes)))
        return;

    if (NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &Length)) &&
        Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
    {
        EtwpDebugLevel = (UCHAR)min(*(PULONG)Info->Data, 0xFF);
    }

    EtwpDebugLevelRead = TRUE;
    ZwClose(Key);
}

/*
 * Some drivers do work in their enable callback that only holds up once they
 * are fully started, which no session at boot ever exercises on Windows.
 * Drivers named in SkipEnableCallback are enabled without being called back.
 */
static
BOOLEAN
EtwpSkipEnableCallback(
    _In_ PVOID Caller)
{
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\WMI\\EtwDebugPrint");
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"SkipEnableCallback");
    PKEY_VALUE_PARTIAL_INFORMATION Info = NULL;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING ImageName;
    UNICODE_STRING Entry;
    WCHAR NameBuffer[64];
    BOOLEAN InSystem;
    BOOLEAN Skip = FALSE;
    KIRQL OldIrql;
    HANDLE Key;
    ULONG Length;
    PWCHAR Cursor;
    PWCHAR End;

    PAGED_CODE();

    RtlInitEmptyUnicodeString(&ImageName, NameBuffer, sizeof(NameBuffer));
    KeAcquireSpinLock(&PsLoadedModuleSpinLock, &OldIrql);
    if (KiPcToFileHeader(Caller, &LdrEntry, FALSE, &InSystem))
        RtlCopyUnicodeString(&ImageName, &LdrEntry->BaseDllName);
    KeReleaseSpinLock(&PsLoadedModuleSpinLock, OldIrql);

    if (!ImageName.Length)
        return FALSE;

    InitializeObjectAttributes(&Attributes, &KeyName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Attributes)))
        return FALSE;

    if (ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, NULL, 0, &Length) == STATUS_BUFFER_TOO_SMALL)
        Info = ExAllocatePoolWithTag(PagedPool, Length, TAG_ETW_PROVIDER);

    if (Info &&
        NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info, Length, &Length)) &&
        Info->Type == REG_MULTI_SZ)
    {
        Cursor = (PWCHAR)Info->Data;
        End = (PWCHAR)(Info->Data + Info->DataLength);
        while (Cursor < End && *Cursor && !Skip)
        {
            RtlInitUnicodeString(&Entry, Cursor);
            Skip = RtlEqualUnicodeString(&Entry, &ImageName, TRUE);
            Cursor += Entry.Length / sizeof(WCHAR) + 1;
        }
    }

    if (Info)
        ExFreePoolWithTag(Info, TAG_ETW_PROVIDER);
    ZwClose(Key);
    return Skip;
}

/* Copies a UTF-8 name, only ASCII survives so the log stays readable at any IRQL */
static
VOID
EtwpCopyName(
    _Out_writes_(Size) PCHAR Destination,
    _In_ ULONG Size,
    _In_reads_bytes_(Available) const UCHAR *Source,
    _In_ ULONG Available)
{
    ULONG Index;

    for (Index = 0; Index + 1 < Size && Index < Available && Source[Index]; Index++)
        Destination[Index] = (Source[Index] < 0x80) ? (CHAR)Source[Index] : '?';

    Destination[Index] = ANSI_NULL;
}

/* Level 0 means always, it is treated as informational */
static
BOOLEAN
EtwpLevelEnabled(
    _In_ PETWP_KERNEL_PROVIDER Provider,
    _In_ UCHAR Level)
{
    if (!Provider->Level)
        return FALSE;

    return (Level ? Level : 4) <= Provider->Level;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETWP_KERNEL_PROVIDER Provider;

    PAGED_CODE();

    if (!ProviderId || !RegHandle)
        return STATUS_INVALID_PARAMETER;

    *RegHandle = 0;

    Provider = ExAllocatePoolZero(NonPagedPool, sizeof(*Provider), TAG_ETW_PROVIDER);
    if (!Provider)
        return STATUS_INSUFFICIENT_RESOURCES;

    EtwpReadDebugLevel();

    Provider->Signature = ETWP_PROVIDER_SIGNATURE;
    Provider->ProviderId = *ProviderId;
    Provider->EnableCallback = EnableCallback;
    Provider->CallbackContext = CallbackContext;
    Provider->Level = EtwpDebugLevel;

    *RegHandle = (REGHANDLE)(ULONG_PTR)Provider;

    /* A provider registering while a session is on learns it right away */
    if (Provider->Level && EnableCallback && !EtwpSkipEnableCallback(_ReturnAddress()))
    {
        /* Control code 1 enables the provider, every keyword is on */
        EnableCallback(ProviderId,
                       1,
                       Provider->Level,
                       ~0ULL,
                       0,
                       NULL,
                       CallbackContext);
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETWP_KERNEL_PROVIDER Provider = (PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle;

    PAGED_CODE();

    if (!EtwpValidProvider(RegHandle))
        return STATUS_INVALID_HANDLE;

    Provider->Signature = 0;
    ExFreePoolWithTag(Provider, TAG_ETW_PROVIDER);
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    if (!EventDescriptor || !EtwpValidProvider(RegHandle))
        return FALSE;

    return EtwpLevelEnabled((PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle, EventDescriptor->Level);
}

BOOLEAN
NTAPI
EtwProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(Keyword);

    if (!EtwpValidProvider(RegHandle))
        return FALSE;

    return EtwpLevelEnabled((PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle, Level);
}

/* Appends one field value to the line */
static
VOID
EtwpFormatValue(
    _Inout_updates_(Size) PCHAR Line,
    _In_ SIZE_T Size,
    _In_ UCHAR InType,
    _In_ PEVENT_DATA_DESCRIPTOR Data)
{
    const UCHAR *Bytes = (const UCHAR *)(ULONG_PTR)Data->Ptr;
    ULONG Length = Data->Size;
    SIZE_T Used = strlen(Line);
    CHAR Text[64];
    ULONG Index;
    WCHAR Char;

    Text[0] = ANSI_NULL;

    if (!Bytes)
    {
        RtlStringCbCopyA(Text, sizeof(Text), "null");
    }
    else
    {
        switch (InType)
        {
            /* UTF-16 text, nul terminated or counted */
            case 1:
            case 22:
                for (Index = 0; Index + 1 < sizeof(Text) && (Index + 1) * sizeof(WCHAR) <= Length; Index++)
                {
                    Char = ((const WCHAR *)Bytes)[Index];
                    if (!Char)
                        break;
                    Text[Index] = (Char < 0x80) ? (CHAR)Char : '?';
                }
                Text[Index] = ANSI_NULL;
                break;

            case 2:
            case 23:
                EtwpCopyName(Text, sizeof(Text), Bytes, Length);
                break;

            case 3:
            case 4:
                RtlStringCbPrintfA(Text, sizeof(Text), "%u", Bytes[0]);
                break;

            case 5:
            case 6:
                if (Length >= sizeof(USHORT))
                    RtlStringCbPrintfA(Text, sizeof(Text), "%u", *(const USHORT *)Bytes);
                break;

            case 7:
                if (Length >= sizeof(LONG))
                    RtlStringCbPrintfA(Text, sizeof(Text), "%ld", *(const LONG *)Bytes);
                break;

            case 8:
            case 13:
                if (Length >= sizeof(ULONG))
                    RtlStringCbPrintfA(Text, sizeof(Text), "%lu", *(const ULONG *)Bytes);
                break;

            case 20:
                if (Length >= sizeof(ULONG))
                    RtlStringCbPrintfA(Text, sizeof(Text), "0x%lx", *(const ULONG *)Bytes);
                break;

            case 9:
            case 10:
            case 16:
            case 17:
            case 21:
                if (Length >= sizeof(ULONG64))
                    RtlStringCbPrintfA(Text, sizeof(Text), "0x%I64x", *(const ULONG64 *)Bytes);
                break;

            case 15:
                if (Length >= sizeof(GUID))
                {
                    RtlStringCbPrintfA(Text,
                                       sizeof(Text),
                                       "{%08lx-%04x-%04x}",
                                       ((const GUID *)Bytes)->Data1,
                                       ((const GUID *)Bytes)->Data2,
                                       ((const GUID *)Bytes)->Data3);
                }
                break;

            /* Anything else shows its first bytes */
            default:
                for (Index = 0; Index < Length && Index < 16; Index++)
                    RtlStringCbPrintfA(Text + Index * 2, sizeof(Text) - Index * 2, "%02x", Bytes[Index]);
                break;
        }
    }

    RtlStringCbPrintfA(Line + Used, Size - Used, "%s", Text);
}

/* Decodes a TraceLogging event, MetadataIndex is the descriptor holding the event metadata */
static
VOID
EtwpPrintTraceLogging(
    _In_ PETWP_KERNEL_PROVIDER Provider,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG DataCount,
    _In_reads_(DataCount) PEVENT_DATA_DESCRIPTOR Data,
    _In_ ULONG MetadataIndex)
{
    const UCHAR *Metadata = (const UCHAR *)(ULONG_PTR)Data[MetadataIndex].Ptr;
    ULONG MetadataSize = Data[MetadataIndex].Size;
    ULONG Offset = sizeof(USHORT);
    ULONG ValueIndex = MetadataIndex + 1;
    CHAR Line[256];
    CHAR Name[ETWP_NAME_LENGTH];
    SIZE_T Used;
    UCHAR InType;

    /* Event tags, a byte with the high bit set has another after it */
    while (Offset < MetadataSize && (Metadata[Offset++] & 0x80));

    EtwpCopyName(Name, sizeof(Name), Metadata + Offset, MetadataSize - Offset);
    while (Offset < MetadataSize && Metadata[Offset])
        Offset++;
    Offset++;

    RtlStringCbPrintfA(Line, sizeof(Line), "ETW %s/%s L%u:", Provider->Name, Name, EventDescriptor->Level);

    while (Offset < MetadataSize && ValueIndex < DataCount)
    {
        EtwpCopyName(Name, sizeof(Name), Metadata + Offset, MetadataSize - Offset);
        while (Offset < MetadataSize && Metadata[Offset])
            Offset++;
        Offset++;
        if (Offset >= MetadataSize)
            break;

        InType = Metadata[Offset++];
        if (InType & ETWP_IN_CHAIN)
        {
            /* The out type, with field tags chained after it */
            while (Offset < MetadataSize && (Metadata[Offset++] & 0x80));
        }
        if (InType & ETWP_IN_CCOUNT)
            Offset += sizeof(USHORT);

        /* Arrays and counted values keep their length in a descriptor of its own */
        if ((InType & ETWP_IN_VCOUNT) ||
            (InType & ETWP_IN_TYPE_MASK) == 14 ||
            (InType & ETWP_IN_TYPE_MASK) == 22 ||
            (InType & ETWP_IN_TYPE_MASK) == 23)
        {
            ValueIndex++;
            if (ValueIndex >= DataCount)
                break;
        }

        Used = strlen(Line);
        RtlStringCbPrintfA(Line + Used, sizeof(Line) - Used, " %s=", Name);
        EtwpFormatValue(Line, sizeof(Line), InType & ETWP_IN_TYPE_MASK, &Data[ValueIndex]);
        ValueIndex++;
    }

    DbgPrint("%s\n", Line);
}

static
VOID
EtwpPrintEvent(
    _In_ PETWP_KERNEL_PROVIDER Provider,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG DataCount,
    _In_reads_opt_(DataCount) PEVENT_DATA_DESCRIPTOR Data)
{
    const UCHAR *Bytes;
    CHAR Hex[3 * 16 + 1];
    ULONG Index;

    for (Index = 0; Data && Index < DataCount && Index < 2; Index++)
    {
        if (!Data[Index].Ptr || Data[Index].Size <= sizeof(USHORT))
            continue;

        if ((Data[Index].Reserved & 0xFF) == ETWP_DATA_PROVIDER_METADATA && !Provider->Name[0])
        {
            EtwpCopyName(Provider->Name,
                         sizeof(Provider->Name),
                         (const UCHAR *)(ULONG_PTR)Data[Index].Ptr + sizeof(USHORT),
                         Data[Index].Size - sizeof(USHORT));
        }

        if ((Data[Index].Reserved & 0xFF) == ETWP_DATA_EVENT_METADATA)
        {
            EtwpPrintTraceLogging(Provider, EventDescriptor, DataCount, Data, Index);
            return;
        }
    }

    /* A manifest event, only its identity and first bytes mean anything here */
    Hex[0] = ANSI_NULL;
    if (Data && DataCount && Data[0].Ptr)
    {
        Bytes = (const UCHAR *)(ULONG_PTR)Data[0].Ptr;
        for (Index = 0; Index < Data[0].Size && Index < 16; Index++)
            RtlStringCbPrintfA(Hex + Index * 3, sizeof(Hex) - Index * 3, "%02x ", Bytes[Index]);
    }

    DbgPrint("ETW %s{%08lx-%04x-%04x} event %u L%u: %s\n",
             Provider->Name,
             Provider->ProviderId.Data1,
             Provider->ProviderId.Data2,
             Provider->ProviderId.Data3,
             EventDescriptor->Id,
             EventDescriptor->Level,
             Hex);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    PETWP_KERNEL_PROVIDER Provider = (PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle;

    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);

    if (!EventDescriptor || UserDataCount > MAX_EVENT_DATA_DESCRIPTORS)
        return STATUS_INVALID_PARAMETER;

    if (!EtwpValidProvider(RegHandle))
        return STATUS_INVALID_HANDLE;

    if (EtwpLevelEnabled(Provider, EventDescriptor->Level))
        EtwpPrintEvent(Provider, EventDescriptor, UserDataCount, UserData);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, NULL, UserDataCount, UserData);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);

    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_opt_ LPCGUID ActivityId,
    _In_ PCWSTR String)
{
    PETWP_KERNEL_PROVIDER Provider = (PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle;
    EVENT_DATA_DESCRIPTOR Data;
    CHAR Line[128];

    UNREFERENCED_PARAMETER(Keyword);
    UNREFERENCED_PARAMETER(ActivityId);

    if (!String)
        return STATUS_INVALID_PARAMETER;

    if (!EtwpValidProvider(RegHandle))
        return STATUS_INVALID_HANDLE;

    if (EtwpLevelEnabled(Provider, Level))
    {
        Data.Ptr = (ULONGLONG)(ULONG_PTR)String;
        Data.Size = (ULONG)(wcslen(String) + 1) * sizeof(WCHAR);
        Data.Reserved = 0;
        Line[0] = ANSI_NULL;
        EtwpFormatValue(Line, sizeof(Line), 1, &Data);
        DbgPrint("ETW %s{%08lx} L%u: %s\n", Provider->Name, Provider->ProviderId.Data1, Level, Line);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
EtwSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    PETWP_KERNEL_PROVIDER Provider = (PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle;

    if (!EtwpValidProvider(RegHandle))
        return STATUS_INVALID_HANDLE;

    /* Provider traits start with their size and the provider name */
    if (InformationClass == EventProviderSetTraits && EventInformation && InformationLength > sizeof(USHORT))
    {
        EtwpCopyName(Provider->Name,
                     sizeof(Provider->Name),
                     (const UCHAR *)EventInformation + sizeof(USHORT),
                     InformationLength - sizeof(USHORT));
    }

    /* Binary tracking and descriptor type only matter to consumers */
    if (InformationClass == 0 || InformationClass == 2 || InformationClass == 3)
        return STATUS_SUCCESS;

    return STATUS_NOT_SUPPORTED;
}

/* Kernel callers keep activity ids themselves, only creation is meaningful here */
_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
NTAPI
EtwActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_updates_bytes_(sizeof(GUID)) LPGUID ActivityId)
{
    static LONG Sequence;
    LARGE_INTEGER Time;
    ULONG Seed;

    if (!ActivityId)
        return STATUS_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
            RtlZeroMemory(ActivityId, sizeof(*ActivityId));
            return STATUS_SUCCESS;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            return STATUS_SUCCESS;

        case EVENT_ACTIVITY_CTRL_CREATE_ID:
        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            KeQuerySystemTime(&Time);
            Seed = Time.LowPart ^ (ULONG)InterlockedIncrement(&Sequence);
            ActivityId->Data1 = RtlRandomEx(&Seed);
            ActivityId->Data2 = (USHORT)RtlRandomEx(&Seed);
            ActivityId->Data3 = (USHORT)((RtlRandomEx(&Seed) & 0x0FFF) | 0x4000);
            *(PULONG)&ActivityId->Data4[0] = RtlRandomEx(&Seed);
            *(PULONG)&ActivityId->Data4[4] = Time.HighPart ^ RtlRandomEx(&Seed);
            ActivityId->Data4[0] = (ActivityId->Data4[0] & 0x3F) | 0x80;
            return STATUS_SUCCESS;

        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            RtlZeroMemory(ActivityId, sizeof(*ActivityId));
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}
