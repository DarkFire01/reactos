/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device objects created with the security their driver asks for
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* What a device class can force on the devices created with its GUID */
typedef struct _IOP_CLASS_CREATION_SETTINGS
{
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    BOOLEAN HasDeviceType;
    BOOLEAN HasCharacteristics;
    BOOLEAN HasExclusive;
    DEVICE_TYPE DeviceType;
    ULONG Characteristics;
    BOOLEAN Exclusive;
} IOP_CLASS_CREATION_SETTINGS, *PIOP_CLASS_CREATION_SETTINGS;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Reads a DWORD value of an open key.
 */
static
BOOLEAN
IopReadClassDword(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value)
{
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING Name;
    ULONG Length;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, ValueName);
    Status = ZwQueryValueKey(KeyHandle, &Name, KeyValuePartialInformation, Info, sizeof(Buffer), &Length);
    if (!NT_SUCCESS(Status) || (Info->Type != REG_DWORD) || (Info->DataLength != sizeof(ULONG)))
        return FALSE;

    *Value = *(PULONG)Info->Data;
    return TRUE;
}

/**
 * @brief Reads the creation settings the Properties key of a device class holds.
 *
 * @param[in] ClassGuid
 * Device class.
 *
 * @param[out] Settings
 * Receives the settings that are present, the descriptor is freed by the caller.
 */
static
VOID
IopReadClassCreationSettings(
    _In_ LPCGUID ClassGuid,
    _Out_ PIOP_CLASS_CREATION_SETTINGS Settings)
{
    UNICODE_STRING GuidString, KeyName, ValueName;
    PKEY_VALUE_PARTIAL_INFORMATION Info = NULL;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE KeyHandle;
    WCHAR KeyBuffer[128];
    ULONG Length, Value;
    NTSTATUS Status;

    RtlZeroMemory(Settings, sizeof(*Settings));

    Status = RtlStringFromGUID(ClassGuid, &GuidString);
    if (!NT_SUCCESS(Status))
        return;

    RtlInitEmptyUnicodeString(&KeyName, KeyBuffer, sizeof(KeyBuffer));
    RtlAppendUnicodeToString(&KeyName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\");
    RtlAppendUnicodeStringToString(&KeyName, &GuidString);
    RtlAppendUnicodeToString(&KeyName, L"\\Properties");
    RtlFreeUnicodeString(&GuidString);

    InitializeObjectAttributes(&ObjectAttributes, &KeyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&KeyHandle, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return;

    if (IopReadClassDword(KeyHandle, L"DeviceType", &Value))
    {
        Settings->HasDeviceType = TRUE;
        Settings->DeviceType = (DEVICE_TYPE)Value;
    }

    if (IopReadClassDword(KeyHandle, L"DeviceCharacteristics", &Value))
    {
        Settings->HasCharacteristics = TRUE;
        Settings->Characteristics = Value;
    }

    if (IopReadClassDword(KeyHandle, L"Exclusive", &Value))
    {
        Settings->HasExclusive = TRUE;
        Settings->Exclusive = (BOOLEAN)(Value != 0);
    }

    /* A descriptor the administrator set for the class wins over the driver's string */
    RtlInitUnicodeString(&ValueName, L"Security");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, NULL, 0, &Length);
    if ((Status == STATUS_BUFFER_TOO_SMALL) || (Status == STATUS_BUFFER_OVERFLOW))
        Info = ExAllocatePoolWithTag(PagedPool, Length, TAG_IO);

    if (Info)
    {
        Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, Info, Length, &Length);
        if (NT_SUCCESS(Status) &&
            (Info->Type == REG_BINARY) &&
            RtlValidRelativeSecurityDescriptor(Info->Data, Info->DataLength, 0))
        {
            Settings->SecurityDescriptor = ExAllocatePoolWithTag(PagedPool, Info->DataLength, TAG_IO);
            if (Settings->SecurityDescriptor)
                RtlCopyMemory(Settings->SecurityDescriptor, Info->Data, Info->DataLength);
        }

        ExFreePoolWithTag(Info, TAG_IO);
    }

    ZwClose(KeyHandle);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Creates a device object with the security its driver describes.
 *
 * @param[in] DriverObject
 * Driver creating the device.
 *
 * @param[in] DeviceExtensionSize
 * Size of the device extension.
 *
 * @param[in] DeviceName
 * Name of the device.
 *
 * @param[in] DeviceType
 * Type of the device, the class may override it.
 *
 * @param[in] DeviceCharacteristics
 * Characteristics of the device, the class may override them.
 *
 * @param[in] Exclusive
 * TRUE for an exclusive device, the class may override it.
 *
 * @param[in] DefaultSDDLString
 * Security of the device in SDDL, unless the class sets one.
 *
 * @param[in] DeviceClassGuid
 * Class whose settings apply, optional.
 *
 * @param[out] DeviceObject
 * Receives the device.
 */
NTSTATUS
NTAPI
IoCreateDeviceSecure(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG DeviceExtensionSize,
    _In_opt_ PUNICODE_STRING DeviceName,
    _In_ DEVICE_TYPE DeviceType,
    _In_ ULONG DeviceCharacteristics,
    _In_ BOOLEAN Exclusive,
    _In_ PCUNICODE_STRING DefaultSDDLString,
    _In_opt_ LPCGUID DeviceClassGuid,
    _Out_ PDEVICE_OBJECT *DeviceObject)
{
    IOP_CLASS_CREATION_SETTINGS Settings;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    SECURITY_INFORMATION SecurityInformation = 0;
    PSID Sid;
    PACL Acl;
    BOOLEAN Present, Defaulted;
    NTSTATUS Status;

    PAGED_CODE();

    *DeviceObject = NULL;

    if (!DefaultSDDLString)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&Settings, sizeof(Settings));
    if (DeviceClassGuid)
        IopReadClassCreationSettings(DeviceClassGuid, &Settings);

    if (Settings.HasDeviceType)
        DeviceType = Settings.DeviceType;
    if (Settings.HasCharacteristics)
        DeviceCharacteristics = Settings.Characteristics;
    if (Settings.HasExclusive)
        Exclusive = Settings.Exclusive;

    SecurityDescriptor = Settings.SecurityDescriptor;
    if (!SecurityDescriptor)
    {
        Status = SepConvertSddlToSecurityDescriptor(DefaultSDDLString, &SecurityDescriptor);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Only the parts the descriptor carries are set on the device */
    if (NT_SUCCESS(RtlGetOwnerSecurityDescriptor(SecurityDescriptor, &Sid, &Defaulted)) && Sid)
        SecurityInformation |= OWNER_SECURITY_INFORMATION;
    if (NT_SUCCESS(RtlGetGroupSecurityDescriptor(SecurityDescriptor, &Sid, &Defaulted)) && Sid)
        SecurityInformation |= GROUP_SECURITY_INFORMATION;
    if (NT_SUCCESS(RtlGetDaclSecurityDescriptor(SecurityDescriptor, &Present, &Acl, &Defaulted)) && Present)
        SecurityInformation |= DACL_SECURITY_INFORMATION;

    Status = IoCreateDevice(DriverObject,
                            DeviceExtensionSize,
                            DeviceName,
                            DeviceType,
                            DeviceCharacteristics,
                            Exclusive,
                            DeviceObject);
    if (NT_SUCCESS(Status) && SecurityInformation)
    {
        Status = ObSetSecurityObjectByPointer(*DeviceObject, SecurityInformation, SecurityDescriptor);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Setting the security of %wZ failed 0x%lx\n", DeviceName, Status);
            IoDeleteDevice(*DeviceObject);
            *DeviceObject = NULL;
        }
    }

    if (Settings.SecurityDescriptor)
        ExFreePoolWithTag(Settings.SecurityDescriptor, TAG_IO);
    else
        SepFreeSddlSecurityDescriptor(SecurityDescriptor);

    return Status;
}
