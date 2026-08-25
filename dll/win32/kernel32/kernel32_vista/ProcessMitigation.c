/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Setting and querying process mitigation policies
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

/*
 * A caller asks for these to harden itself: forced ASLR, strict handle
 * checks, no extension points, Microsoft-signed binaries only, and so on.
 * None of them are enforced here - there is no kernel machinery behind any
 * of them - so a policy that is set is accepted and then has no effect,
 * and a policy that is queried reads back as not enabled, which is the
 * truth about this system rather than an echo of what was set.
 *
 * Accepting them matters because a caller that cannot set a mitigation
 * often refuses to run at all: Chromium terminates its own GPU process
 * with SBOX_FATAL_MITIGATION when SetProcessMitigationPolicy fails, so
 * failing here costs the whole process and buys no security we do not
 * already lack.
 *
 * Enforcing any of these is a kernel change. The information class the
 * real API is built on, ProcessMitigationPolicy, is not implemented in
 * NtSetInformationProcess either.
 */

#include "k32_vista.h"

#include <ndk/psfuncs.h>

#define NDEBUG
#include <debug.h>

/* kernel32 targets an older NT, where winbase.h leaves these out */
#ifndef PROCESS_DEP_ENABLE
#define PROCESS_DEP_ENABLE                      0x00000001
#define PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION 0x00000002
#endif

/*
 * The size the caller must pass for a given policy. Zero means the policy
 * cannot be named here at all.
 */
static
SIZE_T
BasepMitigationPolicySize(
    _In_ PROCESS_MITIGATION_POLICY MitigationPolicy)
{
    switch (MitigationPolicy)
    {
        case ProcessDEPPolicy:
            return sizeof(PROCESS_MITIGATION_DEP_POLICY);
        case ProcessASLRPolicy:
            return sizeof(PROCESS_MITIGATION_ASLR_POLICY);
        case ProcessDynamicCodePolicy:
            return sizeof(PROCESS_MITIGATION_DYNAMIC_CODE_POLICY);
        case ProcessStrictHandleCheckPolicy:
            return sizeof(PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY);
        case ProcessSystemCallDisablePolicy:
            return sizeof(PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY);
        case ProcessExtensionPointDisablePolicy:
            return sizeof(PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY);
        case ProcessControlFlowGuardPolicy:
            return sizeof(PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY);
        case ProcessSignaturePolicy:
            return sizeof(PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY);
        case ProcessFontDisablePolicy:
            return sizeof(PROCESS_MITIGATION_FONT_DISABLE_POLICY);
        case ProcessImageLoadPolicy:
            return sizeof(PROCESS_MITIGATION_IMAGE_LOAD_POLICY);
        case ProcessSystemCallFilterPolicy:
            return sizeof(PROCESS_MITIGATION_SYSTEM_CALL_FILTER_POLICY);
        case ProcessPayloadRestrictionPolicy:
            return sizeof(PROCESS_MITIGATION_PAYLOAD_RESTRICTION_POLICY);
        case ProcessChildProcessPolicy:
            return sizeof(PROCESS_MITIGATION_CHILD_PROCESS_POLICY);
        case ProcessSideChannelIsolationPolicy:
            return sizeof(PROCESS_MITIGATION_SIDE_CHANNEL_ISOLATION_POLICY);
        case ProcessUserShadowStackPolicy:
            return sizeof(PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY);
        case ProcessRedirectionTrustPolicy:
            return sizeof(PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY);

        /* Not a policy in its own right: it names the set of mitigations
           this system can apply, and so is query-only */
        case ProcessMitigationOptionsMask:
        default:
            return 0;
    }
}

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetProcessMitigationPolicy(
    _In_ PROCESS_MITIGATION_POLICY MitigationPolicy,
    _In_reads_bytes_(dwLength) PVOID lpBuffer,
    _In_ SIZE_T dwLength)
{
    SIZE_T Expected;

    if (lpBuffer == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* DEP is set through SetProcessDEPPolicy, not through this, and the
       options mask is query-only. Both are rejected on Windows too. */
    if (MitigationPolicy == ProcessDEPPolicy ||
        MitigationPolicy == ProcessMitigationOptionsMask)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Expected = BasepMitigationPolicySize(MitigationPolicy);
    if (Expected == 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (dwLength != Expected)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Taken, and not enforced. See the note at the top of this file. */
    DPRINT1("Accepting process mitigation policy %d without enforcing it\n",
            MitigationPolicy);

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
GetProcessMitigationPolicy(
    _In_ HANDLE hProcess,
    _In_ PROCESS_MITIGATION_POLICY MitigationPolicy,
    _Out_writes_bytes_(dwLength) PVOID lpBuffer,
    _In_ SIZE_T dwLength)
{
    SIZE_T Expected;

    if (lpBuffer == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (MitigationPolicy == ProcessMitigationOptionsMask)
    {
        /* One ULONG64 per set of PROCESS_CREATION_MITIGATION_POLICY flags.
           Windows accepts a request for the first alone, or for both. */
        if (dwLength != sizeof(ULONG64) && dwLength != sizeof(ULONG64) * 2)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        /* No mitigation can be applied at process creation here. A caller
           that asks this before building a creation policy will then ask
           for none of them, which is what we want it to do. */
        RtlZeroMemory(lpBuffer, dwLength);
        return TRUE;
    }

    Expected = BasepMitigationPolicySize(MitigationPolicy);
    if (Expected == 0 || dwLength != Expected)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlZeroMemory(lpBuffer, dwLength);

    if (MitigationPolicy == ProcessDEPPolicy)
    {
        /* This one has something real behind it */
        PPROCESS_MITIGATION_DEP_POLICY Policy = lpBuffer;
        ULONG ExecuteOptions = 0;
        NTSTATUS Status;

        Status = NtQueryInformationProcess(hProcess,
                                           ProcessExecuteFlags,
                                           &ExecuteOptions,
                                           sizeof(ExecuteOptions),
                                           NULL);
        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            return FALSE;
        }

        Policy->Enable = (ExecuteOptions & MEM_EXECUTE_OPTION_DISABLE) ? 1 : 0;
        Policy->DisableAtlThunkEmulation =
            (ExecuteOptions & MEM_EXECUTE_OPTION_DISABLE_THUNK_EMULATION) ? 1 : 0;
        Policy->Permanent =
            (ExecuteOptions & MEM_EXECUTE_OPTION_PERMANENT) ? TRUE : FALSE;

        return TRUE;
    }

    /* Nothing else is enforced, so nothing else is enabled */
    return TRUE;
}
