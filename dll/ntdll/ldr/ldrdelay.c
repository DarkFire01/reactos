/*
 * PROJECT:     ReactOS NTDLL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Delay load import resolution
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>
#include <delayloadhandler.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* LoadLibraryEx flags a delay load accepts */
#define DLOAD_ALTERED_SEARCH_PATH   0x00000008
#define DLOAD_SEARCH_DLL_LOAD_DIR   0x00000100
#define DLOAD_SAFE_CURRENT_DIRS     0x00002000
#define DLOAD_SEARCH_FLAGS          0x00007F00

/* FUNCTIONS *****************************************************************/

static
BOOLEAN
LdrpIsValidDelayLoadFlags(
    _In_ ULONG Flags)
{
    if ((Flags & ~DLOAD_SAFE_CURRENT_DIRS) == DLOAD_ALTERED_SEARCH_PATH)
        return TRUE;

    return (Flags & ~DLOAD_SEARCH_FLAGS) == 0;
}

/* Builds "<parent directory>;<default path>", or returns NULL to use the default path */
static
PWSTR
LdrpBuildDelayLoadPath(
    _In_ PLDR_DATA_TABLE_ENTRY ParentEntry)
{
    PCWSTR Name = ParentEntry->FullDllName.Buffer;
    SIZE_T DirChars = ParentEntry->FullDllName.Length / sizeof(WCHAR);
    SIZE_T DefaultChars = LdrpDefaultPath.Length / sizeof(WCHAR);
    PWSTR Path;

    while (DirChars && Name[DirChars - 1] != L'\\')
        DirChars--;

    /* Drop the trailing backslash unless it belongs to a root */
    if (DirChars > 1 && Name[DirChars - 2] != L':')
        DirChars--;

    if (!DirChars)
        return NULL;

    Path = RtlAllocateHeap(RtlGetProcessHeap(),
                           0,
                           (DirChars + 1 + DefaultChars + 1) * sizeof(WCHAR));
    if (!Path)
        return NULL;

    RtlCopyMemory(Path, Name, DirChars * sizeof(WCHAR));
    Path[DirChars] = L';';
    RtlCopyMemory(&Path[DirChars + 1], LdrpDefaultPath.Buffer, DefaultChars * sizeof(WCHAR));
    Path[DirChars + 1 + DefaultChars] = UNICODE_NULL;
    return Path;
}

/* Finds or loads the DLL a delay load descriptor names. Called with the loader lock held. */
static
NTSTATUS
LdrpGetDelayLoadTarget(
    _In_ PLDR_DATA_TABLE_ENTRY ParentEntry,
    _In_ PCIMAGE_DELAYLOAD_DESCRIPTOR DelayloadDescriptor,
    _In_ ULONG Flags,
    _Out_ PVOID *TargetBase)
{
    PUCHAR ImageBase = ParentEntry->DllBase;
    PVOID *ModuleHandle = (PVOID *)(ImageBase + DelayloadDescriptor->ModuleHandleRVA);
    PLDR_DATA_TABLE_ENTRY TargetEntry;
    ANSI_STRING AnsiName;
    UNICODE_STRING DllName;
    PWSTR SearchPath = NULL;
    PVOID LoadedBase;
    NTSTATUS Status;

    *TargetBase = NULL;

    /* A handle left by an earlier resolve must still be a loaded module */
    if (*ModuleHandle)
    {
        if (!LdrpCheckForLoadedDllHandle(*ModuleHandle, &TargetEntry))
            return STATUS_DLL_NOT_FOUND;

        *TargetBase = TargetEntry->DllBase;
        return STATUS_SUCCESS;
    }

    RtlInitAnsiString(&AnsiName, (PCSTR)(ImageBase + DelayloadDescriptor->DllNameRVA));
    Status = RtlAnsiStringToUnicodeString(&DllName, &AnsiName, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Flags & (DLOAD_ALTERED_SEARCH_PATH | DLOAD_SEARCH_DLL_LOAD_DIR))
        SearchPath = LdrpBuildDelayLoadPath(ParentEntry);

    Status = LdrLoadDll(SearchPath, NULL, &DllName, &LoadedBase);

    if (SearchPath)
        RtlFreeHeap(RtlGetProcessHeap(), 0, SearchPath);
    RtlFreeUnicodeString(&DllName);

    if (!NT_SUCCESS(Status))
        return Status;

    *ModuleHandle = LoadedBase;
    *TargetBase = LoadedBase;
    return STATUS_SUCCESS;
}

static
PVOID
LdrpCallDelayLoadFailureHooks(
    _In_ PCIMAGE_DELAYLOAD_DESCRIPTOR DelayloadDescriptor,
    _In_opt_ PDELAYLOAD_FAILURE_DLL_CALLBACK FailureDllHook,
    _In_opt_ PDELAYLOAD_FAILURE_SYSTEM_ROUTINE FailureSystemHook,
    _In_ PIMAGE_THUNK_DATA ThunkAddress,
    _In_ PCSTR DllName,
    _In_opt_ PCSTR ProcName,
    _In_ ULONG Ordinal,
    _In_opt_ PVOID TargetBase,
    _In_ NTSTATUS Status)
{
    DELAYLOAD_INFO Info;
    PVOID Result = NULL;

    if (ProcName)
        DPRINT1("Delay load of %s!%s failed with 0x%08lx\n", DllName, ProcName, Status);
    else
        DPRINT1("Delay load of %s!#%lu failed with 0x%08lx\n", DllName, Ordinal, Status);

    if (FailureDllHook)
    {
        RtlZeroMemory(&Info, sizeof(Info));
        Info.Size = sizeof(Info);
        Info.DelayloadDescriptor = DelayloadDescriptor;
        Info.ThunkAddress = ThunkAddress;
        Info.TargetDllName = DllName;
        Info.TargetApiDescriptor.ImportDescribedByName = (ProcName != NULL);
        if (ProcName)
            Info.TargetApiDescriptor.Description.Name = ProcName;
        else
            Info.TargetApiDescriptor.Description.Ordinal = Ordinal;
        Info.TargetModuleBase = TargetBase;
        Info.LastError = RtlNtStatusToDosErrorNoTeb(Status);

        Result = FailureDllHook(DELAYLOAD_GPA_FAILURE, &Info);
    }

    if (!Result && FailureSystemHook)
        Result = FailureSystemHook(DllName, ProcName ? ProcName : (PCSTR)(ULONG_PTR)Ordinal);

    return Result;
}

/*
 * @implemented
 */
PVOID
NTAPI
LdrResolveDelayLoadedAPI(
    _In_ PVOID ParentModuleBase,
    _In_ PCIMAGE_DELAYLOAD_DESCRIPTOR DelayloadDescriptor,
    _In_opt_ PDELAYLOAD_FAILURE_DLL_CALLBACK FailureDllHook,
    _In_opt_ PDELAYLOAD_FAILURE_SYSTEM_ROUTINE FailureSystemHook,
    _Inout_ PIMAGE_THUNK_DATA ThunkAddress,
    _Reserved_ ULONG Flags)
{
    PUCHAR ImageBase = ParentModuleBase;
    PLDR_DATA_TABLE_ENTRY ParentEntry;
    PIMAGE_THUNK_DATA NameThunk;
    PIMAGE_IMPORT_BY_NAME ImportByName;
    ANSI_STRING ProcString;
    PCSTR DllName = NULL;
    PCSTR ProcName = NULL;
    ULONG Ordinal = 0;
    ULONG_PTR Bound;
    SIZE_T Index;
    PVOID TargetBase = NULL;
    PVOID Address = NULL;
    BOOLEAN Failed = FALSE;
    ULONG_PTR Cookie;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!LdrpIsValidDelayLoadFlags(Flags) || !DelayloadDescriptor->Attributes.RvaBased)
        return NULL;

    LdrLockLoaderLock(LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS, NULL, &Cookie);
    _SEH2_TRY
    {
        if (!LdrpCheckForLoadedDllHandle(ParentModuleBase, &ParentEntry))
        {
            DPRINT1("No module is loaded at %p\n", ParentModuleBase);
            _SEH2_LEAVE;
        }

        /* A thunk that left the image is bound already, maybe by another thread */
        Bound = ThunkAddress->u1.Function;
        if (Bound - (ULONG_PTR)ImageBase >= ParentEntry->SizeOfImage)
        {
            Address = (PVOID)Bound;
            _SEH2_LEAVE;
        }

        Index = ThunkAddress - (PIMAGE_THUNK_DATA)(ImageBase + DelayloadDescriptor->ImportAddressTableRVA);
        NameThunk = (PIMAGE_THUNK_DATA)(ImageBase + DelayloadDescriptor->ImportNameTableRVA) + Index;
        if (IMAGE_SNAP_BY_ORDINAL(NameThunk->u1.Ordinal))
        {
            Ordinal = IMAGE_ORDINAL(NameThunk->u1.Ordinal);
        }
        else
        {
            ImportByName = (PIMAGE_IMPORT_BY_NAME)(ImageBase + (ULONG)NameThunk->u1.AddressOfData);
            ProcName = (PCSTR)ImportByName->Name;
        }
        DllName = (PCSTR)(ImageBase + DelayloadDescriptor->DllNameRVA);

        Status = LdrpGetDelayLoadTarget(ParentEntry, DelayloadDescriptor, Flags, &TargetBase);
        if (NT_SUCCESS(Status))
        {
            if (ProcName)
                RtlInitAnsiString(&ProcString, ProcName);

            Status = LdrpGetProcedureAddress(TargetBase,
                                             ProcName ? &ProcString : NULL,
                                             Ordinal,
                                             &Address,
                                             TRUE);

            /* A name missing from the exports is STATUS_PROCEDURE_NOT_FOUND on Windows */
            if (ProcName && Status == STATUS_ENTRYPOINT_NOT_FOUND)
                Status = STATUS_PROCEDURE_NOT_FOUND;
        }

        if (NT_SUCCESS(Status))
        {
            ThunkAddress->u1.Function = (ULONG_PTR)Address;
        }
        else
        {
            Address = NULL;
            Failed = TRUE;
        }
    }
    _SEH2_FINALLY
    {
        LdrUnlockLoaderLock(LDR_UNLOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS, Cookie);
    }
    _SEH2_END;

    if (!Failed)
        return Address;

    /* The hooks run without the loader lock */
    Address = LdrpCallDelayLoadFailureHooks(DelayloadDescriptor,
                                            FailureDllHook,
                                            FailureSystemHook,
                                            ThunkAddress,
                                            DllName,
                                            ProcName,
                                            Ordinal,
                                            TargetBase,
                                            Status);

    /* Only a missing export or an unhosted API set keeps the replacement */
    if (Address &&
        (Status == STATUS_ORDINAL_NOT_FOUND ||
         Status == STATUS_ENTRYPOINT_NOT_FOUND ||
         Status == STATUS_APISET_NOT_HOSTED))
    {
        ThunkAddress->u1.Function = (ULONG_PTR)Address;
    }

    return Address;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
LdrResolveDelayLoadsFromDll(
    _In_ PVOID ParentModuleBase,
    _In_ PCSTR TargetDllName,
    _Reserved_ ULONG Flags)
{
    PUCHAR ImageBase = ParentModuleBase;
    PCIMAGE_DELAYLOAD_DESCRIPTOR Descriptor;
    PIMAGE_THUNK_DATA Thunk;
    ULONG Size;
    ULONG Count;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Flags)
        return STATUS_INVALID_PARAMETER;

    Descriptor = RtlImageDirectoryEntryToData(ParentModuleBase,
                                              TRUE,
                                              IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
                                              &Size);
    if (!Descriptor)
        return STATUS_DLL_NOT_FOUND;

    Count = Size / sizeof(*Descriptor);
    for (i = 0; i < Count && Descriptor->DllNameRVA; i++, Descriptor++)
    {
        if (!_stricmp((PCSTR)(ImageBase + Descriptor->DllNameRVA), TargetDllName))
            break;
    }

    if (i == Count || !Descriptor->DllNameRVA)
        return STATUS_DLL_NOT_FOUND;

    /* Bind every entry, a failed one does not stop the rest */
    Thunk = (PIMAGE_THUNK_DATA)(ImageBase + Descriptor->ImportAddressTableRVA);
    for (; Thunk->u1.Function; Thunk++)
    {
        if (!LdrResolveDelayLoadedAPI(ParentModuleBase, Descriptor, NULL, NULL, Thunk, 0))
            Status = STATUS_DELAY_LOAD_FAILED;
    }

    return Status;
}

/* EOF */
