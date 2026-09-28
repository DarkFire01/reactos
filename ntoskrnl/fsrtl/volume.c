/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What a file system may assume about the volume it sits on
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Set once the session manager has run everything that wanted a raw volume */
BOOLEAN FsRtlpVolumeStartupApplicationsComplete = FALSE;

/* How deep a virtual disk may sit on other virtual disks, once it is known */
static ULONG FsRtlpVirtualDiskMaxTreeDepth = MAXULONG;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Says that nothing is holding a volume open for the sake of checking it.
 *
 * @remarks
 * Called when the session manager is done with what it runs before anything
 * else, which is where a volume check would have been.
 */
VOID
NTAPI
FsRtlSetVolumeStartupApplicationsComplete(VOID)
{
    FsRtlpVolumeStartupApplicationsComplete = TRUE;
}

/**
 * @brief
 * Reads the nesting limit out of one key, leaving what it was given alone if
 * the key or the value is not there.
 */
static
VOID
FsRtlpReadNestingLevel(
    _In_ PCWSTR KeyPath,
    _Inout_ PULONG Depth)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    NTSTATUS Status;
    ULONG Length;
    struct
    {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        UCHAR Room[sizeof(ULONG)];
    } Value;

    RtlInitUnicodeString(&Name, KeyPath);
    InitializeObjectAttributes(&Attributes,
                               &Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwOpenKey(&Key, KEY_READ, &Attributes);
    if (!NT_SUCCESS(Status))
        return;

    RtlInitUnicodeString(&Name, L"VirtualDiskMaxTreeDepth");

    Status = ZwQueryValueKey(Key,
                             &Name,
                             KeyValuePartialInformation,
                             &Value,
                             sizeof(Value),
                             &Length);
    ZwClose(Key);

    if (!NT_SUCCESS(Status) || Value.Info.Type != REG_DWORD)
        return;

    /* Nothing may ask for more nesting than the kernel is willing to walk */
    if (*(PULONG)Value.Info.Data <= *Depth)
        *Depth = *(PULONG)Value.Info.Data;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
FsRtlAreVolumeStartupApplicationsComplete(VOID)
{
    return FsRtlpVolumeStartupApplicationsComplete;
}

/*
 * @implemented
 */
ULONG
NTAPI
FsRtlQueryMaximumVirtualDiskNestingLevel(VOID)
{
    ULONG Depth;

    if (FsRtlpVirtualDiskMaxTreeDepth != MAXULONG)
        return FsRtlpVirtualDiskMaxTreeDepth;

    /* Two deep unless something asks for less, the service first then policy */
    Depth = 2;
    FsRtlpReadNestingLevel(L"\\Registry\\Machine\\System\\CurrentControlSet"
                           L"\\Services\\FsDepends\\Parameters",
                           &Depth);
    FsRtlpReadNestingLevel(L"\\Registry\\Machine\\System\\CurrentControlSet"
                           L"\\Control\\FileSystem\\GroupPolicyKeys",
                           &Depth);

    FsRtlpVirtualDiskMaxTreeDepth = Depth;

    return Depth;
}

/**
 * @brief
 * Tells a driver which of the things a file system can be asked to do are
 * worth asking for on this device.
 *
 * @remarks
 * The answer comes from the filter manager, which offers none of them here,
 * so every caller is told to take the ordinary path.
 */
NTSTATUS
NTAPI
FsRtlGetSupportedFeatures(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ PULONG FeatureSupportFlags)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    *FeatureSupportFlags = 0;

    return STATUS_SUCCESS;
}

/* EOF */
