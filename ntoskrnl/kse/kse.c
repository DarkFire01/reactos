/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Registry backed device flags for driver errata
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

/*
 * Flags are kept under Control\Compatibility\DeviceFlags\<Provider>, one REG_QWORD
 * (or REG_DWORD) value per device, named by the full device key ("USBXHCI:PCI\VEN_...").
 */
#define KSE_FLAGS_ROOT \
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Compatibility\\DeviceFlags\\"

#define KSE_MAX_PROVIDER_LENGTH 64

#define KSE_FLAGS_VALUE_SIZE (FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG64))

/* FUNCTIONS ****************************************************************/

static
BOOLEAN
NTAPI
KsepIsValidProvider(
    _In_ PCWSTR Provider)
{
    size_t Length;

    if (!NT_SUCCESS(RtlStringCchLengthW(Provider, KSE_MAX_PROVIDER_LENGTH + 1, &Length)))
        return FALSE;

    if (Length == 0 || Length > KSE_MAX_PROVIDER_LENGTH)
        return FALSE;

    /* The provider is a single key name, never a path */
    return (wcschr(Provider, OBJ_NAME_PATH_SEPARATOR) == NULL);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
KseQueryDeviceFlags(
    _In_ PCWSTR DeviceKey,
    _In_ PCWSTR Provider,
    _Out_ PULONG64 Flags)
{
    WCHAR PathBuffer[RTL_NUMBER_OF(KSE_FLAGS_ROOT) + KSE_MAX_PROVIDER_LENGTH];
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyPath;
    UNICODE_STRING ValueName;
    ULONG64 ValueBuffer[(KSE_FLAGS_VALUE_SIZE + sizeof(ULONG64) - 1) / sizeof(ULONG64)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)ValueBuffer;
    PUCHAR Data = (PUCHAR)ValueBuffer + FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
    ULONG ResultLength;
    HANDLE KeyHandle;
    NTSTATUS Status;

    PAGED_CODE();

    if (Flags == NULL)
        return STATUS_INVALID_PARAMETER;

    *Flags = 0;

    if (DeviceKey == NULL || Provider == NULL || !KsepIsValidProvider(Provider))
        return STATUS_INVALID_PARAMETER;

    Status = RtlInitUnicodeStringEx(&ValueName, DeviceKey);
    if (!NT_SUCCESS(Status) || ValueName.Length == 0)
        return STATUS_INVALID_PARAMETER;

    Status = RtlStringCbPrintfW(PathBuffer, sizeof(PathBuffer), L"%s%s", KSE_FLAGS_ROOT, Provider);
    if (!NT_SUCCESS(Status))
        return STATUS_INVALID_PARAMETER;

    RtlInitUnicodeString(&KeyPath, PathBuffer);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    /* A missing provider key just means nothing was ever registered for it */
    Status = ZwOpenKey(&KeyHandle, KEY_QUERY_VALUE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return STATUS_NOT_FOUND;

    Status = ZwQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             Value,
                             sizeof(ValueBuffer),
                             &ResultLength);
    ZwClose(KeyHandle);

    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_OBJECT_NAME_NOT_FOUND)
            DPRINT1("Device flags for '%S' unreadable, Status 0x%lx\n", DeviceKey, Status);
        return STATUS_NOT_FOUND;
    }

    if (Value->Type == REG_QWORD && Value->DataLength == sizeof(ULONG64))
    {
        RtlCopyMemory(Flags, Data, sizeof(*Flags));
    }
    else if (Value->Type == REG_DWORD && Value->DataLength == sizeof(ULONG))
    {
        ULONG Low;

        RtlCopyMemory(&Low, Data, sizeof(Low));
        *Flags = Low;
    }
    else
    {
        DPRINT1("Device flags for '%S' have type %lu size %lu, ignored\n",
                DeviceKey, Value->Type, Value->DataLength);
        return STATUS_NOT_FOUND;
    }

    DPRINT("Device flags for '%S' are 0x%I64x\n", DeviceKey, *Flags);
    return STATUS_SUCCESS;
}
