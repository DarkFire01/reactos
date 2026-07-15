/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Em (Errata Manager) functions of Windows 7+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/**
 * @brief
 * Registers an errata-manager rule provider.
 *
 * @param[in] Registration
 * The provider registration descriptor.
 *
 * @param[in] Context
 * Optional provider context passed back to provider callbacks.
 *
 * @param[out] ProviderHandle
 * Receives a handle identifying the registered provider.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * The ReactOS kernel does not implement the errata manager.
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
 * Deregisters a previously registered errata-manager provider.
 *
 * @param[in] ProviderHandle
 * The provider handle returned by EmProviderRegister().
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * The ReactOS kernel does not implement the errata manager.
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
 * Evaluates an errata-manager rule for a client.
 *
 * @param[in] ClientHandle
 * The errata-manager client handle.
 *
 * @param[in] RuleId
 * Identifier of the rule to evaluate.
 *
 * @param[out] Result
 * Receives the result of the rule evaluation.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * The ReactOS kernel does not implement the errata manager.
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
 * Queries the current state of an errata-manager rule for a client.
 *
 * @param[in] ClientHandle
 * The errata-manager client handle.
 *
 * @param[in] RuleId
 * Identifier of the rule to query.
 *
 * @param[out] State
 * Receives the current state of the rule.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * The ReactOS kernel does not implement the errata manager.
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
