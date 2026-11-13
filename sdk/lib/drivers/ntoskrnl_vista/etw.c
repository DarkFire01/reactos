/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Etw functions of Vista+
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 Justin Miller (justin.miller@reactos.org)
 */

/*
 * There is no tracing back end to carry an event to, and no consumer that
 * could read one, so a provider's events go to the debug log instead. That is
 * the only log this system has, and a driver that reports a failure through
 * its provider is otherwise reporting it to nobody.
 *
 * Providers are enabled at the warning level, which is what a driver tests
 * before it builds an event at all. Anything it would only say in passing
 * stays off, so the log keeps the events that are worth reading.
 *
 * The graphics stack fails without a word otherwise, so its providers always
 * print. Everything else prints only once EtwTraceEvents is set.
 */

#include <ntdef.h>
#include <ntifs.h>
#include <ntimage.h>
#include <ntstrsafe.h>
#include <string.h>

#define NDEBUG
#include <debug.h>

/* The PE debug directory, which only the user mode headers carry */
#ifndef IMAGE_DEBUG_TYPE_CODEVIEW
#define IMAGE_DEBUG_TYPE_CODEVIEW 2

typedef struct _IMAGE_DEBUG_DIRECTORY
{
    ULONG Characteristics;
    ULONG TimeDateStamp;
    USHORT MajorVersion;
    USHORT MinorVersion;
    ULONG Type;
    ULONG SizeOfData;
    ULONG AddressOfRawData;
    ULONG PointerToRawData;
} IMAGE_DEBUG_DIRECTORY, *PIMAGE_DEBUG_DIRECTORY;
#endif

/* Exported by the kernel, but no kernel mode header declares it */
NTSYSAPI
PVOID
NTAPI
RtlPcToFileHeader(
    _In_ PVOID PcValue,
    _Out_ PVOID *BaseOfImage);

/* TYPES **********************************************************************/

typedef struct _ETWP_PROVIDER
{
    GUID ProviderId;
    PETWENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
    UCHAR Level;
    BOOLEAN AlwaysPrint;
    CHAR ImageName[24];
} ETWP_PROVIDER, *PETWP_PROVIDER;

/* The CodeView record a linker puts in the debug directory */
typedef struct _ETWP_CODEVIEW_RSDS
{
    ULONG Signature;
    GUID Guid;
    ULONG Age;
    CHAR PdbName[ANYSIZE_ARRAY];
} ETWP_CODEVIEW_RSDS, *PETWP_CODEVIEW_RSDS;

/* GLOBALS ********************************************************************/

/* From evntrace.h, which is a user mode header this cannot reach */
#ifndef EVENT_CONTROL_CODE_DISABLE_PROVIDER
#define EVENT_CONTROL_CODE_DISABLE_PROVIDER 0
#define EVENT_CONTROL_CODE_ENABLE_PROVIDER  1
#endif

#define ETWP_TAG 'wtEK'

/* Warning, which keeps a provider's errors and leaves its chatter behind */
#define ETWP_LEVEL 3

/* How much of one field is worth reading */
#define ETWP_MAXIMUM_FIELD 256

/* Set from the debugger to print events; each driver links its own copy */
BOOLEAN EtwTraceEvents = FALSE;

#define ETWP_RSDS_SIGNATURE 'SDSR'

/* The graphics stack, named by the PDB each image was linked with */
static const PCSTR EtwpAlwaysPrintedImages[] =
{
    "dxgkrnl.pdb",
    "dxgmms1.pdb",
    "dxgmms2.pdb",
    "watchdog.pdb",
    "BasicDisplay.pdb",
    "BasicRender.pdb",
    "nvlddmkm.pdb",
};

/* FUNCTIONS ******************************************************************/

static
PETWP_PROVIDER
EtwpFromHandle(
    _In_ REGHANDLE RegHandle)
{
    return (PETWP_PROVIDER)(ULONG_PTR)RegHandle;
}

/**
 * @brief
 * Finds the file name of the PDB an image was linked with.
 *
 * @param[in] ImageBase
 * The base of the loaded image.
 *
 * @return
 * The name without its path, or NULL when the image carries none.
 */
static
PCSTR
EtwpGetImagePdbName(
    _In_ PVOID ImageBase)
{
    PIMAGE_DOS_HEADER DosHeader = ImageBase;
    PIMAGE_NT_HEADERS NtHeaders;
    PIMAGE_DATA_DIRECTORY Directory;
    PIMAGE_DEBUG_DIRECTORY Debug;
    PETWP_CODEVIEW_RSDS CodeView;
    PCSTR Name;
    ULONG Count;
    ULONG Index;
    ULONG Length;
    ULONG Char;

    if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;

    NtHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)ImageBase + DosHeader->e_lfanew);
    if (NtHeaders->Signature != IMAGE_NT_SIGNATURE)
        return NULL;

    if (NtHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
        return NULL;

    Directory = &NtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if ((Directory->VirtualAddress == 0) || (Directory->Size < sizeof(*Debug)))
        return NULL;

    Debug = (PIMAGE_DEBUG_DIRECTORY)((PUCHAR)ImageBase + Directory->VirtualAddress);
    Count = Directory->Size / sizeof(*Debug);

    for (Index = 0; Index < Count; Index++)
    {
        if ((Debug[Index].Type != IMAGE_DEBUG_TYPE_CODEVIEW) ||
            (Debug[Index].AddressOfRawData == 0) ||
            (Debug[Index].SizeOfData <= FIELD_OFFSET(ETWP_CODEVIEW_RSDS, PdbName)))
        {
            continue;
        }

        CodeView = (PETWP_CODEVIEW_RSDS)((PUCHAR)ImageBase + Debug[Index].AddressOfRawData);
        if (CodeView->Signature != ETWP_RSDS_SIGNATURE)
            continue;

        /* The record says how long it is, so the name is never read past it */
        Length = Debug[Index].SizeOfData - FIELD_OFFSET(ETWP_CODEVIEW_RSDS, PdbName);
        Name = CodeView->PdbName;
        for (Char = 0; (Char < Length) && (CodeView->PdbName[Char] != ANSI_NULL); Char++)
        {
            if ((CodeView->PdbName[Char] == '\\') || (CodeView->PdbName[Char] == '/'))
                Name = &CodeView->PdbName[Char + 1];
        }

        if (Char == Length)
            return NULL;

        return Name;
    }

    return NULL;
}

/**
 * @brief
 * Records which image registered a provider and whether its events print
 * without EtwTraceEvents.
 *
 * @param[in] Provider
 * The provider being registered.
 *
 * @param[in] Caller
 * An address in the image that called EtwRegister().
 */
static
VOID
EtwpIdentifyRegistrant(
    _Inout_ PETWP_PROVIDER Provider,
    _In_ PVOID Caller)
{
    PVOID ImageBase;
    PCSTR PdbName;
    ULONG Index;

    if (RtlPcToFileHeader(Caller, &ImageBase) == NULL)
        return;

    PdbName = EtwpGetImagePdbName(ImageBase);
    if (PdbName == NULL)
        return;

    RtlStringCbCopyA(Provider->ImageName, sizeof(Provider->ImageName), PdbName);

    for (Index = 0; Index < ARRAYSIZE(EtwpAlwaysPrintedImages); Index++)
    {
        if (_stricmp(PdbName, EtwpAlwaysPrintedImages[Index]) == 0)
        {
            Provider->AlwaysPrint = TRUE;
            break;
        }
    }
}

/* Is this field a string the log can just print? */
static
BOOLEAN
EtwpFieldIsAnsiString(
    _In_reads_bytes_(Length) PCSTR Field,
    _In_ ULONG Length)
{
    ULONG Index;

    if ((Length < 2) || (Length > ETWP_MAXIMUM_FIELD))
        return FALSE;

    if (Field[Length - 1] != ANSI_NULL)
        return FALSE;

    for (Index = 0; Index < Length - 1; Index++)
    {
        if ((Field[Index] < ' ') || (Field[Index] > '~'))
            return FALSE;
    }

    return TRUE;
}

/*
 * An event that describes itself carries its own name and the names of its
 * fields packed as a run of strings, which is the only thing that says what
 * the numbers beside them mean.
 */
static
VOID
EtwpPrintNames(
    _In_reads_bytes_(Length) PCSTR Field,
    _In_ ULONG Length)
{
    CHAR Line[ETWP_MAXIMUM_FIELD];
    ULONG Index = 0;
    ULONG Used = 0;

    while (Index < Length)
    {
        ULONG Start = Index;
        ULONG Count;

        while ((Index < Length) && (Field[Index] >= 0x20) && (Field[Index] <= 0x7E))
            Index++;

        Count = Index - Start;
        if ((Count >= 3) && ((Used + Count + 2) < sizeof(Line)))
        {
            if (Used != 0)
                Line[Used++] = ' ';

            RtlCopyMemory(&Line[Used], &Field[Start], Count);
            Used += Count;
        }

        Index++;
    }

    if (Used == 0)
        return;

    Line[Used] = ANSI_NULL;
    DPRINT1("  [%s]\n", Line);
}

/* Writes one field of an event out in whatever shape it turns out to have */
static
VOID
EtwpPrintField(
    _In_ PEVENT_DATA_DESCRIPTOR Data)
{
    PVOID Field = (PVOID)(ULONG_PTR)Data->Ptr;

    if ((Field == NULL) || (Data->Size == 0))
        return;

    if (EtwpFieldIsAnsiString(Field, Data->Size))
    {
        DPRINT1("  %s\n", (PCSTR)Field);
        return;
    }

    switch (Data->Size)
    {
        case sizeof(UCHAR):
            DPRINT1("  %u\n", *(PUCHAR)Field);
            break;

        case sizeof(USHORT):
            DPRINT1("  %u\n", *(PUSHORT)Field);
            break;

        case sizeof(ULONG):
            DPRINT1("  %lu (%lx)\n", *(PULONG)Field, *(PULONG)Field);
            break;

        case sizeof(ULONG64):
            DPRINT1("  %I64u (%I64x)\n", *(PULONG64)Field, *(PULONG64)Field);
            break;

        default:
            DPRINT1("  %lu bytes\n", Data->Size);
            EtwpPrintNames(Field, Data->Size);
            break;
    }
}

/**
 * @brief
 * Tells whether an event is enabled for a provider.
 *
 * @param[in] RegHandle
 * The registration handle returned by EtwRegister().
 *
 * @param[in] EventDescriptor
 * Describes the event being asked about.
 *
 * @return
 * TRUE when the event is at or above the level providers are enabled at.
 */
_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTKRNLVISTAAPI
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    PETWP_PROVIDER Provider = EtwpFromHandle(RegHandle);

    if ((Provider == NULL) || (EventDescriptor == NULL))
        return FALSE;

    /* An event with no level of its own is always worth taking */
    if (EventDescriptor->Level == 0)
        return TRUE;

    return EventDescriptor->Level <= Provider->Level;
}

/**
 * @brief
 * Tells whether any session listens to a provider at a level and keyword.
 *
 * @param[in] RegHandle
 * The registration handle returned by EtwRegister().
 *
 * @param[in] Level
 * The event level being asked about.
 *
 * @param[in] Keyword
 * The event keyword being asked about.
 *
 * @return
 * TRUE when the level is at or above the one providers are enabled at.
 */
_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTKRNLVISTAAPI
NTAPI
EtwProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    PETWP_PROVIDER Provider = EtwpFromHandle(RegHandle);

    UNREFERENCED_PARAMETER(Keyword);

    if (Provider == NULL)
        return FALSE;

    if (Level == 0)
        return TRUE;

    return Level <= Provider->Level;
}

/**
 * @brief
 * Writes an event to the sessions that enabled the provider.
 *
 * @param[in] RegHandle
 * The registration handle returned by EtwRegister().
 *
 * @param[in] EventDescriptor
 * Describes the event to write.
 *
 * @param[in] ActivityId
 * Optional activity to tie the event to.
 *
 * @param[in] UserDataCount
 * Number of entries in @p UserData.
 *
 * @param[in] UserData
 * Optional payload descriptors.
 *
 * @return
 * STATUS_SUCCESS once the event has been written to the debug log.
 */
_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    PETWP_PROVIDER Provider = EtwpFromHandle(RegHandle);
    ULONG Index;

    UNREFERENCED_PARAMETER(ActivityId);

    if ((Provider == NULL) || (!EtwTraceEvents && !Provider->AlwaysPrint))
        return STATUS_SUCCESS;

    if (!EtwEventEnabled(RegHandle, EventDescriptor))
        return STATUS_SUCCESS;

    DPRINT1("Etw: %s event %u level %u keyword %I64x\n",
            Provider->ImageName,
            EventDescriptor->Id,
            EventDescriptor->Level,
            EventDescriptor->Keyword);

    if (UserData == NULL)
        return STATUS_SUCCESS;

    for (Index = 0; Index < UserDataCount; Index++)
        EtwpPrintField(&UserData[Index]);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Registers an event provider.
 *
 * @param[in] ProviderId
 * The GUID of the provider.
 *
 * @param[in] EnableCallback
 * Optional callback run when the provider is enabled or disabled.
 *
 * @param[in] CallbackContext
 * Optional context handed to @p EnableCallback.
 *
 * @param[out] RegHandle
 * Receives the registration handle.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES.
 *
 * @remarks
 * The provider is turned on as it registers, because there is no session that
 * could come along later and turn it on.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETWP_PROVIDER Provider;

    if ((ProviderId == NULL) || (RegHandle == NULL))
        return STATUS_INVALID_PARAMETER;

    Provider = ExAllocatePoolZero(NonPagedPool, sizeof(*Provider), ETWP_TAG);
    if (Provider == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Provider->ProviderId = *ProviderId;
    Provider->EnableCallback = EnableCallback;
    Provider->CallbackContext = CallbackContext;
    Provider->Level = ETWP_LEVEL;
    EtwpIdentifyRegistrant(Provider, _ReturnAddress());

    *RegHandle = (REGHANDLE)(ULONG_PTR)Provider;

    if (EnableCallback != NULL)
    {
        EnableCallback(ProviderId,
                       EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                       Provider->Level,
                       MAXULONGLONG,
                       0,
                       NULL,
                       CallbackContext);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Drops an event provider registration.
 *
 * @param[in] RegHandle
 * The registration handle returned by EtwRegister().
 *
 * @return
 * STATUS_SUCCESS.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTKRNLVISTAAPI
NTAPI
EtwUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETWP_PROVIDER Provider = EtwpFromHandle(RegHandle);

    if (Provider == NULL)
        return STATUS_INVALID_PARAMETER;

    if (Provider->EnableCallback != NULL)
    {
        Provider->EnableCallback(&Provider->ProviderId,
                                 EVENT_CONTROL_CODE_DISABLE_PROVIDER,
                                 0,
                                 0,
                                 0,
                                 NULL,
                                 Provider->CallbackContext);
    }

    ExFreePoolWithTag(Provider, ETWP_TAG);

    return STATUS_SUCCESS;
}
