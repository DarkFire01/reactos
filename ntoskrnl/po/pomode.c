/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Effective power mode notifications
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DATA **********************************************************************/

#define TAG_PO_EFFECTIVE_MODE       'teoP'
#define POP_EFFECTIVE_MODE_SIGNATURE 0xFFEEDDBB

typedef struct _POP_EFFECTIVE_MODE_REGISTRATION
{
    ULONG Version;
    ULONG Signature;
    PEFFECTIVE_POWER_MODE_CALLBACK Callback;
    PVOID Context;
} POP_EFFECTIVE_MODE_REGISTRATION, *PPOP_EFFECTIVE_MODE_REGISTRATION;

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Registers a callback for the effective power mode, and reports the current mode to it.
 *
 * @param[in] Version
 * EFFECTIVE_POWER_MODE_V1 or EFFECTIVE_POWER_MODE_V2.
 *
 * @param[out] RegistrationHandle
 * Receives the handle PoUnregisterFromEffectivePowerModeNotifications takes.
 *
 * @return
 * STATUS_INVALID_PARAMETER for an unknown version or a missing callback or handle.
 *
 * @remarks
 * Nothing changes the power mode yet, so the callback only ever hears Balanced, once.
 */
NTSTATUS
NTAPI
PoRegisterForEffectivePowerModeNotifications(
    _In_ ULONG Version,
    _In_ PEFFECTIVE_POWER_MODE_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Outptr_ PVOID *RegistrationHandle)
{
    PPOP_EFFECTIVE_MODE_REGISTRATION Registration;

    if ((Version != EFFECTIVE_POWER_MODE_V1 && Version != EFFECTIVE_POWER_MODE_V2) ||
        !Callback || !RegistrationHandle)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Registration = ExAllocatePoolZero(NonPagedPool, sizeof(*Registration), TAG_PO_EFFECTIVE_MODE);
    if (!Registration)
        return STATUS_INSUFFICIENT_RESOURCES;

    Registration->Version = Version;
    Registration->Signature = POP_EFFECTIVE_MODE_SIGNATURE;
    Registration->Callback = Callback;
    Registration->Context = Context;
    *RegistrationHandle = Registration;

    /* A new subscriber is told the mode in effect right away */
    Callback(EffectivePowerModeBalanced, Context);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Removes a registration made by PoRegisterForEffectivePowerModeNotifications.
 *
 * @return
 * STATUS_INVALID_PARAMETER for a handle that is not a registration.
 */
NTSTATUS
NTAPI
PoUnregisterFromEffectivePowerModeNotifications(
    _In_ PVOID RegistrationHandle)
{
    PPOP_EFFECTIVE_MODE_REGISTRATION Registration = RegistrationHandle;

    if (!Registration || Registration->Signature != POP_EFFECTIVE_MODE_SIGNATURE)
        return STATUS_INVALID_PARAMETER;

    Registration->Signature = 0;
    ExFreePoolWithTag(Registration, TAG_PO_EFFECTIVE_MODE);
    return STATUS_SUCCESS;
}
