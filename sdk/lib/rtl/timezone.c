/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           Timezone functions
 * FILE:              lib/rtl/timezone.c
 * PROGRAMER:         Eric Kohl
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* Layout of the TZI value and of every year in a zone's Dynamic DST key */
typedef struct _REG_TZI_FORMAT
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    SYSTEMTIME StandardDate;
    SYSTEMTIME DaylightDate;
} REG_TZI_FORMAT, *PREG_TZI_FORMAT;

/* FUNCTIONS ***************************************************************/

static
BOOLEAN
RtlpIsStringTerminated(
    _In_reads_(Count) PCWSTR String,
    _In_ SIZE_T Count)
{
    SIZE_T i;

    for (i = 0; i < Count; i++)
    {
        if (String[i] == UNICODE_NULL)
            return TRUE;
    }

    return FALSE;
}

static
VOID
RtlpSetTimeZoneQuery(
    _Out_ PRTL_QUERY_REGISTRY_TABLE Entry,
    _In_ PCWSTR Name,
    _In_ ULONG Type,
    _In_ PVOID Destination)
{
    Entry->Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Entry->Name = (PWSTR)Name;
    Entry->EntryContext = Destination;
    Entry->DefaultType = Type << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT;
}

/* Copies TimeZoneKeyName, cutting a name that does not fit */
static
NTSTATUS
NTAPI
RtlpReadTimeZoneKeyName(
    _In_ PWSTR ValueName,
    _In_ ULONG ValueType,
    _In_ PVOID ValueData,
    _In_ ULONG ValueLength,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID EntryContext)
{
    PUNICODE_STRING KeyName = EntryContext;
    PCWSTR Source = ValueData;
    SIZE_T Count = 0;
    SIZE_T Limit;

    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Context);

    if (ValueType != REG_SZ)
        return STATUS_SUCCESS;

    Limit = KeyName->MaximumLength / sizeof(WCHAR) - 1;
    if (ValueLength / sizeof(WCHAR) < Limit)
        Limit = ValueLength / sizeof(WCHAR);
    while (Count < Limit && Source[Count] != UNICODE_NULL)
        Count++;

    RtlCopyMemory(KeyName->Buffer, Source, Count * sizeof(WCHAR));
    KeyName->Buffer[Count] = UNICODE_NULL;
    KeyName->Length = (USHORT)(Count * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

/* Reads TimeZoneInformation, with the key name and DST switch when Dynamic is set */
static
NTSTATUS
RtlpQueryTimeZone(
    _Out_ PRTL_DYNAMIC_TIME_ZONE_INFORMATION TimeZone,
    _In_ BOOLEAN Dynamic)
{
    RTL_QUERY_REGISTRY_TABLE QueryTable[10];
    PRTL_QUERY_REGISTRY_TABLE Entry = QueryTable;
    UNICODE_STRING StandardName;
    UNICODE_STRING DaylightName;
    UNICODE_STRING KeyName;
    ULONG DaylightDisabled = 0;
    const LONG DateMarker = -(LONG)sizeof(TIME_FIELDS);
    NTSTATUS Status;

    PAGED_CODE_RTL();

    RtlZeroMemory(TimeZone, Dynamic ? sizeof(RTL_DYNAMIC_TIME_ZONE_INFORMATION) :
                                      sizeof(RTL_TIME_ZONE_INFORMATION));
    RtlZeroMemory(QueryTable, sizeof(QueryTable));

    RtlInitEmptyUnicodeString(&StandardName, TimeZone->StandardName, sizeof(TimeZone->StandardName));
    RtlInitEmptyUnicodeString(&DaylightName, TimeZone->DaylightName, sizeof(TimeZone->DaylightName));

    /* A negative size lets the binary values land in the TIME_FIELDS as they are */
    *(PLONG)&TimeZone->StandardDate = DateMarker;
    *(PLONG)&TimeZone->DaylightDate = DateMarker;

    RtlpSetTimeZoneQuery(Entry++, L"Bias", REG_DWORD, &TimeZone->Bias);
    RtlpSetTimeZoneQuery(Entry++, L"StandardName", REG_SZ, &StandardName);
    RtlpSetTimeZoneQuery(Entry++, L"StandardBias", REG_DWORD, &TimeZone->StandardBias);
    RtlpSetTimeZoneQuery(Entry++, L"StandardStart", REG_BINARY, &TimeZone->StandardDate);
    RtlpSetTimeZoneQuery(Entry++, L"DaylightName", REG_SZ, &DaylightName);
    RtlpSetTimeZoneQuery(Entry++, L"DaylightBias", REG_DWORD, &TimeZone->DaylightBias);
    RtlpSetTimeZoneQuery(Entry++, L"DaylightStart", REG_BINARY, &TimeZone->DaylightDate);

    if (Dynamic)
    {
        RtlInitEmptyUnicodeString(&KeyName, TimeZone->TimeZoneKeyName, sizeof(TimeZone->TimeZoneKeyName));
        Entry->QueryRoutine = RtlpReadTimeZoneKeyName;
        Entry->Name = L"TimeZoneKeyName";
        Entry->EntryContext = &KeyName;
        Entry++;

        RtlpSetTimeZoneQuery(Entry++, L"DynamicDaylightTimeDisabled", REG_DWORD, &DaylightDisabled);
    }

    Status = RtlQueryRegistryValues(RTL_REGISTRY_CONTROL,
                                    L"TimeZoneInformation",
                                    QueryTable,
                                    NULL,
                                    NULL);

    /* A missing start date reads as no date at all */
    if (*(PLONG)&TimeZone->StandardDate == DateMarker)
        *(PLONG)&TimeZone->StandardDate = 0;
    if (*(PLONG)&TimeZone->DaylightDate == DateMarker)
        *(PLONG)&TimeZone->DaylightDate = 0;

    if (NT_SUCCESS(Status) && Dynamic)
        TimeZone->DynamicDaylightTimeDisabled = (DaylightDisabled != 0);

    return Status;
}

static
NTSTATUS
RtlpWriteTimeZoneValue(
    _In_ PCWSTR Name,
    _In_ ULONG Type,
    _In_reads_bytes_(Length) PVOID Data,
    _In_ ULONG Length)
{
    return RtlWriteRegistryValue(RTL_REGISTRY_CONTROL,
                                 L"TimeZoneInformation",
                                 Name,
                                 Type,
                                 Data,
                                 Length);
}

static
ULONG
RtlpStringValueSize(
    _In_ PCWSTR String)
{
    return (ULONG)((wcslen(String) + 1) * sizeof(WCHAR));
}

/* Writes TimeZoneInformation; a legacy caller clears the key name and the dynamic rules */
static
NTSTATUS
RtlpSetTimeZone(
    _In_ PRTL_DYNAMIC_TIME_ZONE_INFORMATION TimeZone,
    _In_ BOOLEAN Dynamic)
{
    PCWSTR KeyName = L"";
    ULONG DaylightDisabled = TRUE;
    NTSTATUS Status;
    NTSTATUS KeyStatus;

    PAGED_CODE_RTL();

    if (!RtlpIsStringTerminated(TimeZone->StandardName, RTL_NUMBER_OF(TimeZone->StandardName)) ||
        !RtlpIsStringTerminated(TimeZone->DaylightName, RTL_NUMBER_OF(TimeZone->DaylightName)) ||
        (Dynamic && !RtlpIsStringTerminated(TimeZone->TimeZoneKeyName,
                                            RTL_NUMBER_OF(TimeZone->TimeZoneKeyName))))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = RtlpWriteTimeZoneValue(L"Bias", REG_DWORD, &TimeZone->Bias, sizeof(LONG));
    if (NT_SUCCESS(Status))
        Status = RtlpWriteTimeZoneValue(L"StandardName", REG_SZ, TimeZone->StandardName,
                                        RtlpStringValueSize(TimeZone->StandardName));
    if (NT_SUCCESS(Status))
        Status = RtlpWriteTimeZoneValue(L"StandardBias", REG_DWORD, &TimeZone->StandardBias, sizeof(LONG));
    if (NT_SUCCESS(Status))
        Status = RtlpWriteTimeZoneValue(L"StandardStart", REG_BINARY, &TimeZone->StandardDate,
                                        sizeof(TIME_FIELDS));
    if (NT_SUCCESS(Status))
        Status = RtlpWriteTimeZoneValue(L"DaylightName", REG_SZ, TimeZone->DaylightName,
                                        RtlpStringValueSize(TimeZone->DaylightName));
    if (NT_SUCCESS(Status))
        Status = RtlpWriteTimeZoneValue(L"DaylightBias", REG_DWORD, &TimeZone->DaylightBias, sizeof(LONG));
    if (NT_SUCCESS(Status))
        Status = RtlpWriteTimeZoneValue(L"DaylightStart", REG_BINARY, &TimeZone->DaylightDate,
                                        sizeof(TIME_FIELDS));

    /* A zone that was not fully written keeps no key name */
    if (NT_SUCCESS(Status) && Dynamic)
    {
        KeyName = TimeZone->TimeZoneKeyName;
        DaylightDisabled = TimeZone->DynamicDaylightTimeDisabled;
    }

    KeyStatus = RtlpWriteTimeZoneValue(L"TimeZoneKeyName", REG_SZ, (PVOID)KeyName, RtlpStringValueSize(KeyName));
    if (NT_SUCCESS(KeyStatus))
        KeyStatus = RtlpWriteTimeZoneValue(L"DynamicDaylightTimeDisabled", REG_DWORD,
                                           &DaylightDisabled, sizeof(ULONG));

    return NT_SUCCESS(Status) ? KeyStatus : Status;
}

/* Reads the rules of one year from Time Zones\<KeyName>\Dynamic DST */
static
NTSTATUS
RtlpQueryDaylightRule(
    _In_ PCWSTR KeyName,
    _In_ USHORT Year,
    _Out_ PREG_TZI_FORMAT Rule)
{
    RTL_QUERY_REGISTRY_TABLE QueryTable[4];
    WCHAR PathBuffer[160];
    WCHAR YearBuffer[8];
    UNICODE_STRING Path;
    UNICODE_STRING YearName;
    ULONG FirstYear = 0;
    ULONG LastYear = 0;
    const LONG RuleMarker = -(LONG)sizeof(REG_TZI_FORMAT);
    NTSTATUS Status;

    if (KeyName[0] == UNICODE_NULL)
        return STATUS_INVALID_PARAMETER;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlAppendUnicodeToString(&Path, L"Time Zones\\");
    if (NT_SUCCESS(Status))
        Status = RtlAppendUnicodeToString(&Path, KeyName);
    if (NT_SUCCESS(Status))
        Status = RtlAppendUnicodeToString(&Path, L"\\Dynamic DST");
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitEmptyUnicodeString(&YearName, YearBuffer, sizeof(YearBuffer));
    Status = RtlIntegerToUnicodeString(Year, 10, &YearName);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    RtlpSetTimeZoneQuery(&QueryTable[0], L"FirstEntry", REG_DWORD, &FirstYear);
    RtlpSetTimeZoneQuery(&QueryTable[1], L"LastEntry", REG_DWORD, &LastYear);
    RtlpSetTimeZoneQuery(&QueryTable[2], YearBuffer, REG_BINARY, Rule);
    QueryTable[0].Flags |= RTL_QUERY_REGISTRY_REQUIRED;
    QueryTable[1].Flags |= RTL_QUERY_REGISTRY_REQUIRED;

    RtlZeroMemory(Rule, sizeof(*Rule));
    Rule->Bias = RuleMarker;

    Status = RtlQueryRegistryValues(RTL_REGISTRY_WINDOWS_NT, PathBuffer, QueryTable, NULL, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Years past the list follow its last rule, years before it the first one */
    if (Year < FirstYear || Year > LastYear)
    {
        RtlInitEmptyUnicodeString(&YearName, YearBuffer, sizeof(YearBuffer));
        Status = RtlIntegerToUnicodeString((Year > LastYear) ? LastYear : FirstYear, 10, &YearName);
        if (!NT_SUCCESS(Status))
            return Status;

        QueryTable[2].Flags |= RTL_QUERY_REGISTRY_REQUIRED;
        Rule->Bias = RuleMarker;
        Status = RtlQueryRegistryValues(RTL_REGISTRY_WINDOWS_NT, PathBuffer, &QueryTable[2], NULL, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return (Rule->Bias == RuleMarker) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_SUCCESS;
}

static
VOID
RtlpCopyCutoverDate(
    _Out_ PTIME_FIELDS Date,
    _In_ const SYSTEMTIME *RegistryDate)
{
    Date->Year = RegistryDate->wYear;
    Date->Month = RegistryDate->wMonth;
    Date->Day = RegistryDate->wDay;
    Date->Hour = RegistryDate->wHour;
    Date->Minute = RegistryDate->wMinute;
    Date->Second = RegistryDate->wSecond;
    Date->Milliseconds = RegistryDate->wMilliseconds;
    Date->Weekday = RegistryDate->wDayOfWeek;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlQueryTimeZoneInformation(
    _Out_ PRTL_TIME_ZONE_INFORMATION TimeZoneInformation)
{
    DPRINT("RtlQueryTimeZoneInformation()\n");

    return RtlpQueryTimeZone((PRTL_DYNAMIC_TIME_ZONE_INFORMATION)TimeZoneInformation, FALSE);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlQueryDynamicTimeZoneInformation(
    _Out_ PRTL_DYNAMIC_TIME_ZONE_INFORMATION TimeZoneInformation)
{
    return RtlpQueryTimeZone(TimeZoneInformation, TRUE);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlSetTimeZoneInformation(
    _In_ PRTL_TIME_ZONE_INFORMATION TimeZoneInformation)
{
    DPRINT("RtlSetTimeZoneInformation()\n");

    return RtlpSetTimeZone((PRTL_DYNAMIC_TIME_ZONE_INFORMATION)TimeZoneInformation, FALSE);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlSetDynamicTimeZoneInformation(
    _In_ PRTL_DYNAMIC_TIME_ZONE_INFORMATION TimeZoneInformation)
{
    return RtlpSetTimeZone(TimeZoneInformation, TRUE);
}

/*
 * @implemented
 * Puts the Dynamic DST rules of Year into TimeZoneInformation.
 * Returns TRUE when that changed any of its biases or dates.
 */
BOOLEAN
NTAPI
RtlpCheckDynamicTimeZoneInformation(
    _Inout_ PRTL_DYNAMIC_TIME_ZONE_INFORMATION TimeZoneInformation,
    _In_ USHORT Year)
{
    RTL_TIME_ZONE_INFORMATION YearZone;
    REG_TZI_FORMAT Rule;

    PAGED_CODE_RTL();

    if (!RtlpIsStringTerminated(TimeZoneInformation->TimeZoneKeyName,
                                RTL_NUMBER_OF(TimeZoneInformation->TimeZoneKeyName)))
    {
        return FALSE;
    }

    if (!NT_SUCCESS(RtlpQueryDaylightRule(TimeZoneInformation->TimeZoneKeyName, Year, &Rule)))
        return FALSE;

    RtlCopyMemory(&YearZone, TimeZoneInformation, sizeof(YearZone));
    YearZone.Bias = Rule.Bias;
    YearZone.StandardBias = Rule.StandardBias;
    YearZone.DaylightBias = Rule.DaylightBias;
    RtlpCopyCutoverDate(&YearZone.StandardDate, &Rule.StandardDate);
    RtlpCopyCutoverDate(&YearZone.DaylightDate, &Rule.DaylightDate);

    if (RtlEqualMemory(&YearZone, TimeZoneInformation, sizeof(YearZone)))
        return FALSE;

    RtlCopyMemory(TimeZoneInformation, &YearZone, sizeof(YearZone));
    return TRUE;
}

/* EOF */
