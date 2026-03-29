/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT User Mode Library
 * FILE:            dll/ntdll/ldr/ldrdelay.c
 * PURPOSE:         Delay-load descriptor resolution (Vista+)
 */

#include <ntdll.h>

/* PE delay-load descriptor (winnt); keep layout in sync with the loader */
typedef struct _IMAGE_DELAYLOAD_DESCRIPTOR {
    ULONG Attributes;
    ULONG DllNameRVA;
    ULONG ModuleHandleRVA;
    ULONG ImportAddressTableRVA;
    ULONG ImportNameTableRVA;
    ULONG BoundImportAddressTableRVA;
    ULONG UnloadInformationTableRVA;
    ULONG TimeDateStamp;
} IMAGE_DELAYLOAD_DESCRIPTOR;
typedef const IMAGE_DELAYLOAD_DESCRIPTOR *PCIMAGE_DELAYLOAD_DESCRIPTOR;

#include <delayloadhandler.h>

#define NDEBUG
#include <debug.h>

static FORCEINLINE PVOID
LdrpDelayloadVaFromRva(_In_ PVOID Base, _In_ ULONG Rva)
{
    return Rva ? (PVOID)((ULONG_PTR)Base + Rva) : NULL;
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
    _In_ PIMAGE_THUNK_DATA ThunkAddress,
    _In_ ULONG Flags)
{
    PIMAGE_THUNK_DATA pIAT, pINT;
    DELAYLOAD_INFO DelayInfo;
    UNICODE_STRING Mod;
    const CHAR *Name;
    HMODULE *PhModule;
    NTSTATUS Status;
    FARPROC Proc;
    INT_PTR Index;

    (void)Flags;

    PhModule = LdrpDelayloadVaFromRva(ParentModuleBase, DelayloadDescriptor->ModuleHandleRVA);
    pIAT = LdrpDelayloadVaFromRva(ParentModuleBase, DelayloadDescriptor->ImportAddressTableRVA);
    pINT = LdrpDelayloadVaFromRva(ParentModuleBase, DelayloadDescriptor->ImportNameTableRVA);
    Name = LdrpDelayloadVaFromRva(ParentModuleBase, DelayloadDescriptor->DllNameRVA);

    if (!PhModule || !pIAT || !pINT || !Name || !ThunkAddress)
        return NULL;

    Index = (INT_PTR)(ThunkAddress - pIAT);

    if (!*PhModule)
    {
        if (!RtlCreateUnicodeStringFromAsciiz(&Mod, Name))
        {
            Status = STATUS_NO_MEMORY;
            goto fail;
        }
        Status = LdrLoadDll(NULL, NULL, &Mod, (PVOID *)PhModule);
        RtlFreeUnicodeString(&Mod);
        if (!NT_SUCCESS(Status))
            goto fail;
    }

    if (IMAGE_SNAP_BY_ORDINAL(pINT[Index].u1.Ordinal))
    {
        Status = LdrGetProcedureAddress(*PhModule, NULL, LOWORD(pINT[Index].u1.Ordinal), (PVOID *)&Proc);
    }
    else
    {
        const IMAGE_IMPORT_BY_NAME *ImportByName =
            LdrpDelayloadVaFromRva(ParentModuleBase, PtrToUlong(pINT[Index].u1.AddressOfData));
        ANSI_STRING Fn;

        if (!ImportByName)
        {
            Status = STATUS_INVALID_PARAMETER;
            goto fail;
        }
        RtlInitAnsiString(&Fn, (PCSZ)ImportByName->Name);
        Status = LdrGetProcedureAddress(*PhModule, &Fn, 0, (PVOID *)&Proc);
    }

    if (NT_SUCCESS(Status))
    {
        pIAT[Index].u1.Function = (ULONG_PTR)Proc;
        return (PVOID)Proc;
    }

fail:
    DelayInfo.Size = sizeof(DelayInfo);
    DelayInfo.DelayloadDescriptor = DelayloadDescriptor;
    DelayInfo.ThunkAddress = ThunkAddress;
    DelayInfo.TargetDllName = Name;
    DelayInfo.TargetApiDescriptor.ImportDescribedByName = !IMAGE_SNAP_BY_ORDINAL(pINT[Index].u1.Ordinal);
    DelayInfo.TargetApiDescriptor.Description.Ordinal = LOWORD(pINT[Index].u1.Ordinal);
    DelayInfo.TargetModuleBase = *PhModule;
    DelayInfo.Unused = NULL;
    DelayInfo.LastError = (ULONG)Status;

    if (FailureDllHook)
        return FailureDllHook(DELAYLOAD_GPA_FAILURE, &DelayInfo);

    if (!FailureSystemHook)
        return NULL;

    if (IMAGE_SNAP_BY_ORDINAL(pINT[Index].u1.Ordinal))
    {
        ULONG_PTR Ord = LOWORD(pINT[Index].u1.Ordinal);
        return FailureSystemHook(Name, (LPCSTR)Ord);
    }
    else
    {
        const IMAGE_IMPORT_BY_NAME *ImportByName =
            LdrpDelayloadVaFromRva(ParentModuleBase, PtrToUlong(pINT[Index].u1.AddressOfData));

        if (!ImportByName)
            return NULL;
        return FailureSystemHook(Name, (LPCSTR)ImportByName->Name);
    }
}
