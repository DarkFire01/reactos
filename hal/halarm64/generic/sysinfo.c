/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HAL system information classes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/**
 * @brief Answers the information classes this HAL implements.
 *
 * @return STATUS_INVALID_LEVEL for every other class.
 */
NTSTATUS
NTAPI
HaliQuerySystemInformation(
    _In_ HAL_QUERY_INFORMATION_CLASS InformationClass,
    _In_ ULONG BufferSize,
    _Inout_ PVOID Buffer,
    _Out_ PULONG ReturnedLength)
{
    *ReturnedLength = 0;

    switch (InformationClass)
    {
        case HalSecondaryInterruptInformation:
            return HalpQuerySecondaryInformation(Buffer, BufferSize, ReturnedLength);

        default:
            return STATUS_INVALID_LEVEL;
    }
}

/**
 * @brief Takes the settings this HAL implements.
 *
 * @return STATUS_INVALID_LEVEL for every other class.
 */
NTSTATUS
NTAPI
HaliSetSystemInformation(
    _In_ HAL_SET_INFORMATION_CLASS InformationClass,
    _In_ ULONG BufferSize,
    _In_ PVOID Buffer)
{
    switch (InformationClass)
    {
        case HalRegisterSecondaryInterruptInterface:
            return HalpRegisterSecondaryProvider(Buffer, BufferSize);

        default:
            return STATUS_INVALID_LEVEL;
    }
}
