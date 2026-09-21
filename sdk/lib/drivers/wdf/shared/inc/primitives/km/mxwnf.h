/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WNF subscription primitive, for a kernel without WNF
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The framework only uses WNF to learn when DRIPS constraints are registered,
 * which decides whether Sleep Study tracks a device. There is no WNF here, so
 * every call fails and the framework leaves Sleep Study off for the device.
 */

#pragma once

#ifndef _WNF_STATE_NAME_DEFINED
#define _WNF_STATE_NAME_DEFINED
typedef struct _WNF_STATE_NAME
{
    ULONG Data[2];
} WNF_STATE_NAME, *PWNF_STATE_NAME;
typedef const WNF_STATE_NAME *PCWNF_STATE_NAME;
#endif

/* Never looked up, the subscription below always fails first. */
#define WNF_PO_DRIPS_DEVICE_CONSTRAINTS_REGISTERED { 0, 0 }

typedef struct _MxWnfSubscriptionContext *PMxWnfSubscriptionContext;

typedef
NTSTATUS
MX_WNF_CALLBACK(
    _In_ PMxWnfSubscriptionContext SubscriptionContext,
    _In_opt_ PVOID CallbackContext);

typedef MX_WNF_CALLBACK *PFN_MX_WNF_CALLBACK;

class MxWnf
{
public:
    static
    __inline
    NTSTATUS
    MxSubscribeWnfStateChange(
        _Out_ PMxWnfSubscriptionContext *SubscriptionContext,
        _In_ PCWNF_STATE_NAME StateName,
        _In_ PFN_MX_WNF_CALLBACK Callback,
        _In_opt_ PVOID CallbackContext,
        _In_opt_ PVOID Tag)
    {
        UNREFERENCED_PARAMETER(StateName);
        UNREFERENCED_PARAMETER(Callback);
        UNREFERENCED_PARAMETER(CallbackContext);
        UNREFERENCED_PARAMETER(Tag);

        *SubscriptionContext = NULL;
        return STATUS_NOT_SUPPORTED;
    }

    static
    __inline
    VOID
    MxUnsubscribeWnfStateChange(
        _Inout_ PMxWnfSubscriptionContext *SubscriptionContext)
    {
        *SubscriptionContext = NULL;
    }

    static
    __inline
    NTSTATUS
    MxQueryWnfStateData(
        _In_opt_ PMxWnfSubscriptionContext SubscriptionContext,
        _Out_writes_bytes_(*BufferSize) PVOID Buffer,
        _Inout_ PULONG BufferSize)
    {
        UNREFERENCED_PARAMETER(SubscriptionContext);
        UNREFERENCED_PARAMETER(Buffer);
        UNREFERENCED_PARAMETER(BufferSize);

        return STATUS_NOT_SUPPORTED;
    }
};
