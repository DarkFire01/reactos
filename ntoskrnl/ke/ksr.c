/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel Soft Reboot support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * A soft reboot hands the next kernel the memory and metadata the previous one set aside, so a
 * display driver can keep scanning out of the same framebuffer across the restart. Nothing here
 * boots that way, so every query reports that there is nothing to inherit and the callers take
 * their ordinary path.
 *
 * These are reached through the ext-ms-win-ntos-ksr-l1-1 api set, which has no host module of
 * its own: the kernel answers it directly.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/**
 * @brief Walks the memory ranges a previous kernel persisted across a soft reboot.
 *
 * @return STATUS_NOT_FOUND, there having been no soft reboot to inherit from.
 */
NTSTATUS
NTAPI
KsrEnumeratePersistedMemory(
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Callback,
    _In_opt_ PVOID CallbackContext)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(CallbackContext);

    return STATUS_NOT_FOUND;
}

/**
 * @brief Releases a range that was inherited from a soft reboot.
 *
 * Nothing was ever handed out, so there is nothing to give back.
 */
VOID
NTAPI
KsrFreePersistedMemory(
    _In_opt_ PVOID BaseAddress)
{
    UNREFERENCED_PARAMETER(BaseAddress);
}

/**
 * @brief Reports what the firmware left behind for a soft reboot.
 *
 * @return STATUS_NOT_SUPPORTED, since no soft reboot path runs here.
 */
NTSTATUS
NTAPI
KsrGetFirmwareInformation(
    _In_opt_ PVOID Information,
    _In_ ULONG Length)
{
    UNREFERENCED_PARAMETER(Information);
    UNREFERENCED_PARAMETER(Length);

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Sets aside a driver's metadata for the kernel that follows a soft reboot.
 *
 * @return STATUS_NOT_SUPPORTED, so the caller keeps its state itself.
 */
NTSTATUS
NTAPI
KsrPersistMetadata(
    _In_opt_ PVOID Key,
    _In_opt_ PVOID Data,
    _In_ ULONG Length)
{
    UNREFERENCED_PARAMETER(Key);
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(Length);

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Reads back metadata a previous kernel persisted.
 *
 * @return STATUS_NOT_FOUND, nothing having been persisted.
 */
NTSTATUS
NTAPI
KsrQueryMetadata(
    _In_opt_ PVOID Key,
    _Out_opt_ PVOID Data,
    _Inout_opt_ PULONG Length)
{
    UNREFERENCED_PARAMETER(Key);
    UNREFERENCED_PARAMETER(Data);

    if (Length != NULL)
        *Length = 0;

    return STATUS_NOT_FOUND;
}

/* EOF */
