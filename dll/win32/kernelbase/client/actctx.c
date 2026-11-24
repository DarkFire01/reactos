/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/actctx.c
 * PURPOSE:         Activation contexts - NT-compatible helpers
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *
 * NOTE:            See also kernel32/wine/actctx.c
 */

/* INCLUDES ******************************************************************/

#include <k32.h>
#define NDEBUG
#include <debug.h>

#define QUERY_ACTCTX_FLAG_VALID         (QUERY_ACTCTX_FLAG_USE_ACTIVE_ACTCTX | \
                                         QUERY_ACTCTX_FLAG_ACTCTX_IS_HMODULE | \
                                         QUERY_ACTCTX_FLAG_ACTCTX_IS_ADDRESS | \
                                         QUERY_ACTCTX_FLAG_NO_ADDREF)

/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
BasepFreeActivationContextActivationBlock(IN PBASEP_ACTCTX_BLOCK ActivationBlock)
{
    /* Exit if there was nothing passed in */
    if (!ActivationBlock) return;

    /* Do we have a context? */
    if (ActivationBlock->ActivationContext)
    {
        /* Release and clear it */
        RtlReleaseActivationContext(ActivationBlock->ActivationContext);
        ActivationBlock->ActivationContext = NULL;
    }

    /* Free the block */
    RtlFreeHeap(RtlGetProcessHeap(), 0, ActivationBlock);
}

NTSTATUS
NTAPI
BasepAllocateActivationContextActivationBlock(IN DWORD Flags,
                                              IN PVOID CompletionRoutine,
                                              IN PVOID CompletionContext,
                                              OUT PBASEP_ACTCTX_BLOCK *ActivationBlock)
{
    NTSTATUS Status;
    ACTIVATION_CONTEXT_BASIC_INFORMATION ContextInfo;

    /* Clear the info structure */
    ContextInfo.dwFlags = 0;
    ContextInfo.hActCtx = NULL;

    /* Assume failure */
    if (ActivationBlock) *ActivationBlock = NULL;

    /* Only support valid flags */
    if (Flags & ~(1 | 2)) // FIXME: What are they? 2 looks like BASEP_ACTCTX_FORCE_BLOCK
    {
        /* Fail if unknown flags are passed in */
        Status = STATUS_INVALID_PARAMETER_1;
        goto Quickie;
    }

    /* Caller should have passed in an activation block */
    if (!ActivationBlock)
    {
        /* Fail otherwise */
        Status = STATUS_INVALID_PARAMETER_4;
        goto Quickie;
    }

    /* Query RTL for information on the current activation context */
    Status = RtlQueryInformationActivationContext(RTL_QUERY_ACTIVATION_CONTEXT_FLAG_USE_ACTIVE_ACTIVATION_CONTEXT,
                                                  NULL,
                                                  NULL,
                                                  ActivationContextBasicInformation,
                                                  &ContextInfo,
                                                  sizeof(ContextInfo),
                                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        /* Failed -- bail out */
        DPRINT1("SXS: %s - Failure getting active activation context; ntstatus %08lx\n",
                __FUNCTION__, Status);
        goto Quickie;
    }

    /* Check if the current one should be freed */
    if (ContextInfo.dwFlags & 1)
    {
        /* Release and clear it */
        RtlReleaseActivationContext(ContextInfo.hActCtx);
        ContextInfo.hActCtx = NULL;
    }

    /* Check if there's an active context, or if the caller is forcing one */
    if (!(Flags & 2) || (ContextInfo.hActCtx))
    {
        /* Allocate the block */
        *ActivationBlock = RtlAllocateHeap(RtlGetProcessHeap(),
                                           0,
                                           sizeof(BASEP_ACTCTX_BLOCK));
        if (!(*ActivationBlock))
        {
            /* Ran out of memory, fail */
            Status = STATUS_NO_MEMORY;
            goto Quickie;
        }

        /* Fill it out */
        (*ActivationBlock)->ActivationContext = ContextInfo.hActCtx;
        (*ActivationBlock)->Flags = 0;
        if (Flags & 1) (*ActivationBlock)->Flags |= 1; // Not sure about this flag
        (*ActivationBlock)->CompletionRoutine = CompletionRoutine;
        (*ActivationBlock)->CompletionContext = CompletionContext;

        /* Tell Quickie below not to free anything, since this is success */
        ContextInfo.hActCtx = NULL;
    }

    /* Set success status */
    Status = STATUS_SUCCESS;

Quickie:
    /* Failure or success path, return to caller and free on failure */
    if (ContextInfo.hActCtx) RtlReleaseActivationContext(ContextInfo.hActCtx);
    return Status;
}

NTSTATUS
NTAPI
BasepProbeForDllManifest(IN PVOID DllHandle,
                         IN PCWSTR FullDllName,
                         OUT PVOID *ActCtx)
{
    NTSTATUS Status = STATUS_SUCCESS;
    LDR_RESOURCE_INFO Info;
    IMAGE_RESOURCE_DATA_ENTRY *Entry;
    ACTCTXW Context;
    HANDLE Result;

    /* Check if activation context parameter is provided */
    if (!ActCtx)
    {
        ASSERT(FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    /* Zero it out */
    *ActCtx = NULL;

    /* Check whether the image has manifest resource associated with it */
    Info.Type = (ULONG_PTR)RT_MANIFEST;
    Info.Name = (ULONG_PTR)ISOLATIONAWARE_MANIFEST_RESOURCE_ID;
    Info.Language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    if (!(Status = LdrFindResource_U(DllHandle, &Info, 3, &Entry)))
    {
        /* Create the activation context */
        Context.cbSize = sizeof(Context);
        Context.lpSource = FullDllName;
        Context.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
        Context.hModule = DllHandle;
        Context.lpResourceName = (LPCWSTR)ISOLATIONAWARE_MANIFEST_RESOURCE_ID;

        Status = RtlCreateActivationContext(0, (PVOID)&Context, 0, NULL, NULL, &Result);

        /* Store activation context pointer if it was created successfully */
        if (NT_SUCCESS(Status)) *ActCtx = Result;

        /* CORE-10843: Windows always returns this since we pass the wrong struct */
        if (Status == STATUS_SXS_INVALID_ACTCTXDATA_FORMAT)
        {
            /* Fake "Manifest not found" so the load doesn't fail  */
            static int Once;
            if (Once++)
            {
                DPRINT1("HACK: Passed invalid ACTIVATION_CONTEXT_DATA!\n");
            }
            Status = STATUS_RESOURCE_DATA_NOT_FOUND;
        }
    }

    return Status;
}

/* PUBLIC FUNCTIONS **********************************************************/
/***********************************************************************
 * CreateActCtxW (KERNEL32.@)
 *
 * Create an activation context.
 */
HANDLE WINAPI CreateActCtxW(PCACTCTXW pActCtx)
{
    NTSTATUS status;
    HANDLE hActCtx;

    if ((status = RtlCreateActivationContext(0, (PVOID)pActCtx, 0, NULL, NULL, &hActCtx)))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return INVALID_HANDLE_VALUE;
    }
    return hActCtx;
}

/* EOF */
