/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Em (Errata Manager) functions of Windows 7+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Registers an errata rule provider.
 *
 * @param[in] Registration
 * The provider registration descriptor.
 *
 * @param[in] Context
 * Optional context handed back to the provider callbacks.
 *
 * @param[out] ProviderHandle
 * Receives the handle of the registered provider.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
EmProviderRegister(
    _In_ PVOID Registration,
    _In_opt_ PVOID Context,
    _Out_ PVOID *ProviderHandle)
{
    UNREFERENCED_PARAMETER(Registration);
    UNREFERENCED_PARAMETER(Context);

    if (ProviderHandle != NULL)
        *ProviderHandle = NULL;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Deregisters an errata rule provider.
 *
 * @param[in] ProviderHandle
 * The handle returned by EmProviderRegister().
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
EmProviderDeregister(
    _In_ PVOID ProviderHandle)
{
    UNREFERENCED_PARAMETER(ProviderHandle);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Evaluates an errata rule on behalf of a client.
 *
 * @param[in] ClientHandle
 * The errata manager client handle.
 *
 * @param[in] RuleId
 * Identifier of the rule to evaluate.
 *
 * @param[out] Result
 * Receives the outcome of the evaluation.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
EmClientRuleEvaluate(
    _In_ PVOID ClientHandle,
    _In_ PVOID RuleId,
    _Out_ PVOID Result)
{
    UNREFERENCED_PARAMETER(ClientHandle);
    UNREFERENCED_PARAMETER(RuleId);
    UNREFERENCED_PARAMETER(Result);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Queries the current state of an errata rule.
 *
 * @param[in] ClientHandle
 * The errata manager client handle.
 *
 * @param[in] RuleId
 * Identifier of the rule to query.
 *
 * @param[out] State
 * Receives the state of the rule.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 *
 * @unimplemented
 */
NTSTATUS
NTAPI
EmClientQueryRuleState(
    _In_ PVOID ClientHandle,
    _In_ PVOID RuleId,
    _Out_ PVOID State)
{
    UNREFERENCED_PARAMETER(ClientHandle);
    UNREFERENCED_PARAMETER(RuleId);
    UNREFERENCED_PARAMETER(State);

    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
