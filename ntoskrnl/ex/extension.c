/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver registration with kernel extension hosts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DATA **********************************************************************/

/* The only registration layout ExRegisterExtension accepts */
#define EXP_EXTENSION_REGISTRATION_V1   0x10001

typedef struct _EXP_EXTENSION_REGISTRATION
{
    USHORT ExtensionId;
    USHORT ExtensionVersion;
    USHORT Reserved;
    USHORT FunctionCount;
    PVOID FunctionTable;
    PVOID *HostInterface;
    PVOID DriverObject;
} EXP_EXTENSION_REGISTRATION, *PEXP_EXTENSION_REGISTRATION;

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Attaches a driver's function table to the kernel host that serves its extension id.
 *
 * @param[out] Extension
 * Receives the extension, for ExUnregisterExtension.
 *
 * @param[in] RegistrationVersion
 * Layout of the registration, only version 1 exists.
 *
 * @return
 * STATUS_INVALID_PARAMETER for an unknown layout or a function count without a table,
 * STATUS_NOT_FOUND when no host serves the extension id and version.
 *
 * @remarks
 * No kernel component registers an extension host yet, so every registration ends at the lookup.
 */
NTSTATUS
NTAPI
ExRegisterExtension(
    _Outptr_ PVOID *Extension,
    _In_ ULONG RegistrationVersion,
    _In_ PVOID RegistrationInfo)
{
    PEXP_EXTENSION_REGISTRATION Registration = RegistrationInfo;

    UNREFERENCED_PARAMETER(Extension);

    if ((RegistrationVersion != EXP_EXTENSION_REGISTRATION_V1) ||
        (!Registration->FunctionTable && Registration->FunctionCount))
    {
        return STATUS_INVALID_PARAMETER;
    }

    DPRINT1("No host for extension %u version %u\n",
            Registration->ExtensionId, Registration->ExtensionVersion);
    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * Detaches an extension ExRegisterExtension returned.
 *
 * @remarks
 * ExRegisterExtension never hands one out while no host exists, so there is nothing to undo.
 */
VOID
NTAPI
ExUnregisterExtension(
    _In_ PVOID Extension)
{
    UNREFERENCED_PARAMETER(Extension);
}
