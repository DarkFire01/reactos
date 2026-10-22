/*
 * PROJECT:     ReactOS RTL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Dynamic function table support routines
 * COPYRIGHT:   Copyright 2022-2025 Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

#define TAG_RTLDYNFNTBL 'tfDP'

typedef
_Function_class_(GET_RUNTIME_FUNCTION_CALLBACK)
PRUNTIME_FUNCTION
GET_RUNTIME_FUNCTION_CALLBACK(
    _In_ DWORD64 ControlPc,
    _In_opt_ PVOID Context);
typedef GET_RUNTIME_FUNCTION_CALLBACK *PGET_RUNTIME_FUNCTION_CALLBACK;

typedef
_Function_class_(OUT_OF_PROCESS_FUNCTION_TABLE_CALLBACK)
DWORD
OUT_OF_PROCESS_FUNCTION_TABLE_CALLBACK(
    _In_ HANDLE Process,
    _In_ PVOID TableAddress,
    _Out_ PDWORD Entries,
    _Out_ PRUNTIME_FUNCTION* Functions);
typedef OUT_OF_PROCESS_FUNCTION_TABLE_CALLBACK *POUT_OF_PROCESS_FUNCTION_TABLE_CALLBACK;

typedef enum _FUNCTION_TABLE_TYPE
{
    RF_SORTED = 0x0,
    RF_UNSORTED = 0x1,
    RF_CALLBACK = 0x2,
    RF_KERNEL_DYNAMIC = 0x3,
} FUNCTION_TABLE_TYPE;

typedef struct _DYNAMIC_FUNCTION_TABLE
{
    LIST_ENTRY ListEntry;
    PRUNTIME_FUNCTION FunctionTable;
    LARGE_INTEGER TimeStamp;
    ULONG64 MinimumAddress;
    ULONG64 MaximumAddress;
    ULONG64 BaseAddress;
    PGET_RUNTIME_FUNCTION_CALLBACK Callback;
    PVOID Context;
    PWCHAR OutOfProcessCallbackDll;
    FUNCTION_TABLE_TYPE Type;
    ULONG EntryCount;
#if (NTDDI_VERSION <= NTDDI_WIN10)
    // FIXME: RTL_BALANCED_NODE is defined in ntdef.h, it's impossible to get included here due to precompiled header
    //RTL_BALANCED_NODE TreeNode;
#else
    //RTL_BALANCED_NODE TreeNodeMin;
    //RTL_BALANCED_NODE TreeNodeMax;
#endif
} DYNAMIC_FUNCTION_TABLE, *PDYNAMIC_FUNCTION_TABLE;

RTL_SRWLOCK RtlpDynamicFunctionTableLock = { 0 };
LIST_ENTRY RtlpDynamicFunctionTableList = { &RtlpDynamicFunctionTableList, &RtlpDynamicFunctionTableList };

static __inline
VOID
AcquireDynamicFunctionTableLockExclusive()
{
    RtlAcquireSRWLockExclusive(&RtlpDynamicFunctionTableLock);
}

static __inline
VOID
ReleaseDynamicFunctionTableLockExclusive()
{
    RtlReleaseSRWLockExclusive(&RtlpDynamicFunctionTableLock);
}

static __inline
VOID
AcquireDynamicFunctionTableLockShared()
{
    RtlAcquireSRWLockShared(&RtlpDynamicFunctionTableLock);
}

static __inline
VOID
ReleaseDynamicFunctionTableLockShared()
{
    RtlReleaseSRWLockShared(&RtlpDynamicFunctionTableLock);
}

/*
 * https://docs.microsoft.com/en-us/windows/win32/devnotes/rtlgetfunctiontablelisthead 
 */
PLIST_ENTRY
NTAPI
RtlGetFunctionTableListHead(void)
{
    return &RtlpDynamicFunctionTableList;
}

static
VOID
RtlpInsertDynamicFunctionTable(PDYNAMIC_FUNCTION_TABLE DynamicTable)
{
    //LARGE_INTEGER TimeStamp;

    AcquireDynamicFunctionTableLockExclusive();

    /* Insert it into the list */
    InsertTailList(&RtlpDynamicFunctionTableList, &DynamicTable->ListEntry);

    // TODO: insert into RB-trees

    ReleaseDynamicFunctionTableLockExclusive();
}

BOOLEAN
NTAPI
RtlAddFunctionTable(
    _In_ PRUNTIME_FUNCTION FunctionTable,
    _In_ DWORD EntryCount,
    _In_ DWORD64 BaseAddress)
{
    PDYNAMIC_FUNCTION_TABLE dynamicTable;
    ULONG i;

    /* Allocate a dynamic function table */
    dynamicTable = RtlpAllocateMemory(sizeof(*dynamicTable), TAG_RTLDYNFNTBL);
    if (dynamicTable == NULL)
    {
        DPRINT1("Failed to allocate dynamic function table\n");
        return FALSE;
    }

    /* Initialize fields */
    dynamicTable->FunctionTable = FunctionTable;
    dynamicTable->EntryCount = EntryCount;
    dynamicTable->BaseAddress = BaseAddress;
    dynamicTable->Callback = NULL;
    dynamicTable->Context = NULL;
    dynamicTable->Type = RF_UNSORTED;

    /* Loop all entries to find the margins */
    dynamicTable->MinimumAddress = ULONG64_MAX;
    dynamicTable->MaximumAddress = 0;
    for (i = 0; i < EntryCount; i++)
    {
        dynamicTable->MinimumAddress = min(dynamicTable->MinimumAddress,
                                           FunctionTable[i].BeginAddress);
        dynamicTable->MaximumAddress = max(dynamicTable->MaximumAddress,
                                           FunctionTable[i].EndAddress);
    }

    /* Adjust the margins to be absolute addresses */
    dynamicTable->MinimumAddress += BaseAddress;
    dynamicTable->MaximumAddress += BaseAddress;

    /* Insert the table into the list */
    RtlpInsertDynamicFunctionTable(dynamicTable);

    return TRUE;
}

BOOLEAN
NTAPI
RtlInstallFunctionTableCallback(
    _In_ DWORD64 TableIdentifier,
    _In_ DWORD64 BaseAddress,
    _In_ DWORD Length,
    _In_ PGET_RUNTIME_FUNCTION_CALLBACK Callback,
    _In_ PVOID Context,
    _In_opt_z_ PCWSTR OutOfProcessCallbackDll)
{
    PDYNAMIC_FUNCTION_TABLE dynamicTable;
    SIZE_T stringLength, allocationSize;

    /* Make sure the identifier is valid */
    if ((TableIdentifier & 3) != 3)
    {
        return FALSE;
    }

    /* Check if we have a DLL name */
    if (OutOfProcessCallbackDll != NULL)
    {
        stringLength = wcslen(OutOfProcessCallbackDll) + 1;
    }
    else
    {
        stringLength = 0;
    }

    /* Calculate required size */
    allocationSize = sizeof(DYNAMIC_FUNCTION_TABLE) + stringLength * sizeof(WCHAR);

    /* Allocate a dynamic function table */
    dynamicTable = RtlpAllocateMemory(allocationSize, TAG_RTLDYNFNTBL);
    if (dynamicTable == NULL)
    {
        DPRINT1("Failed to allocate dynamic function table\n");
        return FALSE;
    }

    /* Initialize fields */
    dynamicTable->FunctionTable = (PRUNTIME_FUNCTION)TableIdentifier;
    dynamicTable->EntryCount = 0;
    dynamicTable->BaseAddress = BaseAddress;
    dynamicTable->Callback = Callback;
    dynamicTable->Context = Context;
    dynamicTable->Type = RF_CALLBACK;
    dynamicTable->MinimumAddress = BaseAddress;
    dynamicTable->MaximumAddress = BaseAddress + Length;

    /* If we have a DLL name, copy that, too */
    if (OutOfProcessCallbackDll != NULL)
    {
        dynamicTable->OutOfProcessCallbackDll = (PWCHAR)(dynamicTable + 1);
        RtlCopyMemory(dynamicTable->OutOfProcessCallbackDll,
                      OutOfProcessCallbackDll,
                      stringLength * sizeof(WCHAR));
    }
    else
    {
        dynamicTable->OutOfProcessCallbackDll = NULL;
    }

    /* Insert the table into the list */
    RtlpInsertDynamicFunctionTable(dynamicTable);

    return TRUE;
}

BOOLEAN
NTAPI
RtlDeleteFunctionTable(
    _In_ PRUNTIME_FUNCTION FunctionTable)
{
    PLIST_ENTRY listLink;
    PDYNAMIC_FUNCTION_TABLE dynamicTable;
    BOOL removed = FALSE;

    AcquireDynamicFunctionTableLockExclusive();

    /* Loop all tables to find the one to delete */
    for (listLink = RtlpDynamicFunctionTableList.Flink;
         listLink != &RtlpDynamicFunctionTableList;
         listLink = listLink->Flink)
    {
        dynamicTable = CONTAINING_RECORD(listLink, DYNAMIC_FUNCTION_TABLE, ListEntry);

        if (dynamicTable->FunctionTable == FunctionTable)
        {
            RemoveEntryList(&dynamicTable->ListEntry);
            removed = TRUE;
            break;
        }
    }

    ReleaseDynamicFunctionTableLockExclusive();

    /* If we were successful, free the memory */
    if (removed)
    {
        RtlpFreeMemory(dynamicTable, TAG_RTLDYNFNTBL);
    }

    return removed;
}

/**
 * @brief Registers a sorted function table over a fixed range that the caller fills over time.
 */
DWORD
NTAPI
RtlAddGrowableFunctionTable(
    _Out_ PVOID *DynamicTable,
    _In_reads_(MaximumEntryCount) PRUNTIME_FUNCTION FunctionTable,
    _In_ DWORD EntryCount,
    _In_ DWORD MaximumEntryCount,
    _In_ ULONG_PTR RangeBase,
    _In_ ULONG_PTR RangeEnd)
{
    PDYNAMIC_FUNCTION_TABLE dynamicTable, otherTable;
    PLIST_ENTRY listLink;
    NTSTATUS status = STATUS_SUCCESS;

    if ((EntryCount > MaximumEntryCount) || (RangeBase >= RangeEnd))
        RtlRaiseStatus(STATUS_INVALID_PARAMETER);

    dynamicTable = RtlpAllocateMemory(sizeof(*dynamicTable), TAG_RTLDYNFNTBL);
    if (dynamicTable == NULL)
        return STATUS_NO_MEMORY;

    /* Entries are relative to the start of the range */
    dynamicTable->FunctionTable = FunctionTable;
    dynamicTable->EntryCount = EntryCount;
    dynamicTable->BaseAddress = RangeBase;
    dynamicTable->MinimumAddress = RangeBase;
    dynamicTable->MaximumAddress = RangeEnd;
    dynamicTable->Callback = NULL;
    dynamicTable->Context = NULL;
    dynamicTable->OutOfProcessCallbackDll = NULL;

    /* Growable tables share the type used for kernel dynamic tables */
    dynamicTable->Type = RF_KERNEL_DYNAMIC;

    AcquireDynamicFunctionTableLockExclusive();

    /* The first table touching this range decides: an identical range is fine, a partial one is not */
    for (listLink = RtlpDynamicFunctionTableList.Flink;
         listLink != &RtlpDynamicFunctionTableList;
         listLink = listLink->Flink)
    {
        otherTable = CONTAINING_RECORD(listLink, DYNAMIC_FUNCTION_TABLE, ListEntry);

        if ((RangeEnd <= otherTable->MinimumAddress) ||
            (RangeBase >= otherTable->MaximumAddress))
        {
            continue;
        }

        if ((otherTable->MinimumAddress != RangeBase) ||
            (otherTable->MaximumAddress != RangeEnd))
        {
            status = STATUS_CONFLICTING_ADDRESSES;
        }
        break;
    }

    if (NT_SUCCESS(status))
        InsertTailList(&RtlpDynamicFunctionTableList, &dynamicTable->ListEntry);

    ReleaseDynamicFunctionTableLockExclusive();

    if (!NT_SUCCESS(status))
    {
        RtlpFreeMemory(dynamicTable, TAG_RTLDYNFNTBL);
        return status;
    }

    *DynamicTable = dynamicTable;
    return STATUS_SUCCESS;
}

/**
 * @brief Makes entries the caller has already written to a growable table visible to unwinding.
 */
VOID
NTAPI
RtlGrowFunctionTable(
    _Inout_ PVOID DynamicTable,
    _In_ DWORD NewEntryCount)
{
    PDYNAMIC_FUNCTION_TABLE dynamicTable = DynamicTable;

    if ((dynamicTable->Type != RF_KERNEL_DYNAMIC) ||
        (NewEntryCount < dynamicTable->EntryCount))
    {
        RtlRaiseStatus(STATUS_INVALID_PARAMETER);
    }

    dynamicTable->EntryCount = NewEntryCount;
}

/**
 * @brief Unregisters and frees a table created by RtlAddGrowableFunctionTable.
 */
VOID
NTAPI
RtlDeleteGrowableFunctionTable(
    _In_ PVOID DynamicTable)
{
    PDYNAMIC_FUNCTION_TABLE dynamicTable = DynamicTable;

    if (dynamicTable->Type != RF_KERNEL_DYNAMIC)
        RtlRaiseStatus(STATUS_INVALID_PARAMETER);

    AcquireDynamicFunctionTableLockExclusive();
    RemoveEntryList(&dynamicTable->ListEntry);
    ReleaseDynamicFunctionTableLockExclusive();

    RtlpFreeMemory(dynamicTable, TAG_RTLDYNFNTBL);
}

PRUNTIME_FUNCTION
NTAPI
RtlpLookupDynamicFunctionEntry(
    _In_ DWORD64 ControlPc,
    _Out_ PDWORD64 ImageBase,
    _In_ PUNWIND_HISTORY_TABLE HistoryTable)
{
    PLIST_ENTRY listLink;
    PDYNAMIC_FUNCTION_TABLE dynamicTable;
    PRUNTIME_FUNCTION functionTable, foundEntry = NULL;
    PGET_RUNTIME_FUNCTION_CALLBACK callback;
    DWORD64 ipOffset;
    ULONG i;

    AcquireDynamicFunctionTableLockShared();

    /* Loop all tables to find the one matching ControlPc */
    for (listLink = RtlpDynamicFunctionTableList.Flink;
         listLink != &RtlpDynamicFunctionTableList;
         listLink = listLink->Flink)
    {
        dynamicTable = CONTAINING_RECORD(listLink, DYNAMIC_FUNCTION_TABLE, ListEntry);

        if ((ControlPc >= dynamicTable->MinimumAddress) &&
            (ControlPc < dynamicTable->MaximumAddress))
        {
            /* Check if there is a callback */
            callback = dynamicTable->Callback;
            if (callback != NULL)
            {
                PVOID context = dynamicTable->Context;

                *ImageBase = dynamicTable->BaseAddress;
                ReleaseDynamicFunctionTableLockShared();
                return callback(ControlPc, context);
            }

            /* Loop all entries in the function table */
            functionTable = dynamicTable->FunctionTable;
            ipOffset = ControlPc - dynamicTable->BaseAddress;
            for (i = 0; i < dynamicTable->EntryCount; i++)
            {
                /* Check if this entry contains the address */
                if ((ipOffset >= functionTable[i].BeginAddress) &&
                    (ipOffset < functionTable[i].EndAddress))
                {
                    foundEntry = &functionTable[i];
                    *ImageBase = dynamicTable->BaseAddress;
                    goto Exit;
                }
            }
        }
    }

Exit:

    ReleaseDynamicFunctionTableLockShared();

    return foundEntry;
}
