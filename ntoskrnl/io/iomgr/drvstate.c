/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver and device state directories and registry keys
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Remembers the directory name picked for a device across boots */
static const DEVPROPKEY IopStateDirectoryKey =
{
    { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } },
    21
};

static UNICODE_STRING IopServicesKeyPath =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services");
static UNICODE_STRING IopServiceStateRoot = RTL_CONSTANT_STRING(L"\\SystemRoot\\ServiceState");
static UNICODE_STRING IopDriverStateRoot = RTL_CONSTANT_STRING(L"\\SystemRoot\\System32\\DriverState");
static UNICODE_STRING IopDevicesName = RTL_CONSTANT_STRING(L"Devices");
static UNICODE_STRING IopDataName = RTL_CONSTANT_STRING(L"Data");
static UNICODE_STRING IopSharedDataName = RTL_CONSTANT_STRING(L"SharedData");
static UNICODE_STRING IopParametersName = RTL_CONSTANT_STRING(L"Parameters");
static UNICODE_STRING IopStateName = RTL_CONSTANT_STRING(L"State");
static UNICODE_STRING IopSharedStateName = RTL_CONSTANT_STRING(L"SharedState");

#define IOP_STATE_DIRECTORY_ACCESS (FILE_LIST_DIRECTORY | SYNCHRONIZE)
#define IOP_STATE_DIRECTORY_SHARE (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define IOP_STATE_DIRECTORY_OPTIONS (FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)

/* PRIVATE FUNCTIONS **********************************************************/

static
BOOLEAN
IopIsDriverObjectUsable(
    _In_opt_ PDRIVER_OBJECT DriverObject)
{
    return (DriverObject &&
            DriverObject->DriverExtension &&
            DriverObject->DriverExtension->ServiceKeyName.Buffer &&
            DriverObject->DriverExtension->ServiceKeyName.Length);
}

/**
 * @brief
 * Joins path components with backslashes into a new pool string.
 */
static
NTSTATUS
IopJoinPath(
    _Out_ PUNICODE_STRING Path,
    _In_reads_(Count) PCUNICODE_STRING *Components,
    _In_ ULONG Count)
{
    ULONG Length = 0;
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
        Length += Components[Index]->Length + sizeof(WCHAR);

    if (Length > UNICODE_STRING_MAX_BYTES)
        return STATUS_BUFFER_OVERFLOW;

    Path->Buffer = ExAllocatePoolWithTag(PagedPool, Length, TAG_IO);
    if (!Path->Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    Path->Length = 0;
    Path->MaximumLength = (USHORT)Length;

    for (Index = 0; Index < Count; Index++)
    {
        if (Index)
            RtlAppendUnicodeToString(Path, L"\\");

        RtlAppendUnicodeStringToString(Path, Components[Index]);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Opens a directory, optionally creating every missing component of its path.
 */
static
NTSTATUS
IopOpenStateDirectory(
    _In_ PUNICODE_STRING Path,
    _In_ BOOLEAN Create,
    _Out_ PHANDLE DirectoryHandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING Prefix;
    HANDLE Directory = NULL;
    NTSTATUS Status;
    USHORT Index;

    InitializeObjectAttributes(&ObjectAttributes,
                               Path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&Directory,
                        IOP_STATE_DIRECTORY_ACCESS,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        IOP_STATE_DIRECTORY_SHARE,
                        IOP_STATE_DIRECTORY_OPTIONS);

    if (Status != STATUS_OBJECT_PATH_NOT_FOUND && Status != STATUS_OBJECT_NAME_NOT_FOUND)
    {
        if (NT_SUCCESS(Status))
            *DirectoryHandle = Directory;
        return Status;
    }

    if (!Create)
        return Status;

    Prefix.Buffer = Path->Buffer;
    Prefix.MaximumLength = Path->Length;

    for (Index = 1; Index <= Path->Length / sizeof(WCHAR); Index++)
    {
        if (Index < Path->Length / sizeof(WCHAR) && Path->Buffer[Index] != L'\\')
            continue;

        Prefix.Length = Index * sizeof(WCHAR);
        InitializeObjectAttributes(&ObjectAttributes,
                                   &Prefix,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL,
                                   NULL);

        if (Directory)
            ZwClose(Directory);

        Status = ZwCreateFile(&Directory,
                              IOP_STATE_DIRECTORY_ACCESS,
                              &ObjectAttributes,
                              &IoStatusBlock,
                              NULL,
                              FILE_ATTRIBUTE_NORMAL,
                              IOP_STATE_DIRECTORY_SHARE,
                              FILE_OPEN_IF,
                              IOP_STATE_DIRECTORY_OPTIONS,
                              NULL,
                              0);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *DirectoryHandle = Directory;
    return STATUS_SUCCESS;
}

static
NTSTATUS
IopGetDriverImageDirectory(
    _In_ PDRIVER_OBJECT DriverObject,
    _Out_ PHANDLE DirectoryHandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING Path;
    NTSTATUS Status;
    WCHAR Character;

    Status = IoQueryFullDriverPath(DriverObject, &Path);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Drop the file name, keeping the directory it sits in */
    do
    {
        if (Path.Length < sizeof(WCHAR))
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }

        Path.Length -= sizeof(WCHAR);
        Character = Path.Buffer[Path.Length / sizeof(WCHAR)];
    } while (Character != L'\\' && Character != L'/');

    if (Path.Length <= sizeof(WCHAR))
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    InitializeObjectAttributes(&ObjectAttributes,
                               &Path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(DirectoryHandle,
                        IOP_STATE_DIRECTORY_ACCESS,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        IOP_STATE_DIRECTORY_SHARE,
                        IOP_STATE_DIRECTORY_OPTIONS);

Exit:
    ExFreePoolWithTag(Path.Buffer, TAG_IO);
    return Status;
}

static
NTSTATUS
IopOpenServiceKey(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE ServiceKey)
{
    HANDLE ServicesKey;
    NTSTATUS Status;

    Status = IopOpenRegistryKeyEx(&ServicesKey, NULL, &IopServicesKeyPath, KEY_READ);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IopOpenRegistryKeyEx(ServiceKey,
                                  ServicesKey,
                                  &DriverObject->DriverExtension->ServiceKeyName,
                                  DesiredAccess);
    ZwClose(ServicesKey);
    return Status;
}

/**
 * @brief
 * Checks that a service key belongs to a driver and not to a Win32 service.
 */
static
NTSTATUS
IopCheckServiceIsDriver(
    _In_ HANDLE ServiceKey)
{
    PKEY_VALUE_FULL_INFORMATION Information;
    NTSTATUS Status;
    ULONG Type;

    Status = IopGetRegistryValue(ServiceKey, L"Type", &Information);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_INVALID_PARAMETER;

    if (!NT_SUCCESS(Status))
        return Status;

    if (Information->Type != REG_DWORD || Information->DataLength != sizeof(Type))
    {
        ExFreePool(Information);
        return STATUS_REGISTRY_CORRUPT;
    }

    Type = *(PULONG)((ULONG_PTR)Information + Information->DataOffset);
    ExFreePool(Information);

    if (!(Type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)))
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

static
BOOLEAN
IopIsStateNameCharacter(
    _In_ WCHAR Character)
{
    if (Character < L' ')
        return FALSE;

    return (wcschr(L"\"*+,/:;<=>?[\\]|", Character) == NULL);
}

/**
 * @brief
 * Turns a device instance path into a directory name.
 *
 * @return
 * STATUS_INVALID_PARAMETER when the path holds characters past ASCII.
 */
static
NTSTATUS
IopMakeDeviceStateName(
    _In_ PCUNICODE_STRING InstancePath,
    _Out_ PUNICODE_STRING Name)
{
    NTSTATUS Status;
    USHORT Index;

    Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
                                       InstancePath,
                                       Name);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = 0; Index < Name->Length / sizeof(WCHAR); Index++)
    {
        if (Name->Buffer[Index] > 0x7F)
        {
            RtlFreeUnicodeString(Name);
            return STATUS_INVALID_PARAMETER;
        }

        if (!IopIsStateNameCharacter(Name->Buffer[Index]))
            Name->Buffer[Index] = L'#';
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the directory name stored for a device by an earlier call.
 *
 * @return
 * STATUS_OBJECT_NAME_NOT_FOUND when nothing usable is stored.
 */
static
NTSTATUS
IopQueryDeviceStateName(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PUNICODE_STRING Name)
{
    DEVPROPTYPE Type;
    ULONG Required = 0;
    PWCHAR Buffer;
    NTSTATUS Status;
    ULONG Index;

    Status = IoGetDevicePropertyData(PhysicalDeviceObject,
                                     &IopStateDirectoryKey,
                                     LOCALE_NEUTRAL,
                                     0,
                                     0,
                                     NULL,
                                     &Required,
                                     &Type);
    if (Status == STATUS_NOT_FOUND)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (Status != STATUS_BUFFER_TOO_SMALL)
        return NT_SUCCESS(Status) ? STATUS_OBJECT_NAME_NOT_FOUND : Status;

    if (Type != DEVPROP_TYPE_STRING || Required < sizeof(WCHAR) || Required > UNICODE_STRING_MAX_BYTES)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    Buffer = ExAllocatePoolWithTag(PagedPool, Required, TAG_IO);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoGetDevicePropertyData(PhysicalDeviceObject,
                                     &IopStateDirectoryKey,
                                     LOCALE_NEUTRAL,
                                     0,
                                     Required,
                                     Buffer,
                                     &Required,
                                     &Type);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Buffer, TAG_IO);
        return Status;
    }

    Buffer[Required / sizeof(WCHAR) - 1] = UNICODE_NULL;

    /* A stored name that would not make a valid directory is ignored */
    for (Index = 0; Buffer[Index] != UNICODE_NULL; Index++)
    {
        if (Buffer[Index] > 0x7F || !IopIsStateNameCharacter(Buffer[Index]))
        {
            ExFreePoolWithTag(Buffer, TAG_IO);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    RtlInitUnicodeString(Name, Buffer);
    return STATUS_SUCCESS;
}

static
NTSTATUS
IopOpenDeviceStateDirectory(
    _In_ PUNICODE_STRING Name,
    _In_ BOOLEAN Create,
    _Out_ PHANDLE DirectoryHandle)
{
    PCUNICODE_STRING Components[4];
    UNICODE_STRING Path;
    NTSTATUS Status;

    Components[0] = &IopDriverStateRoot;
    Components[1] = &IopDevicesName;
    Components[2] = Name;
    Components[3] = &IopDataName;

    Status = IopJoinPath(&Path, Components, RTL_NUMBER_OF(Components));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IopOpenStateDirectory(&Path, Create, DirectoryHandle);
    ExFreePoolWithTag(Path.Buffer, TAG_IO);
    return Status;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Opens one of the registry keys a driver keeps its settings and state in.
 *
 * @param[in] DriverObject
 * The driver.
 *
 * @param[in] RegKeyType
 * Parameters is opened read only. PersistentState and SharedPersistentState
 * are created on first use.
 *
 * @param[in] DesiredAccess
 * Access to the key.
 *
 * @param[in] Flags
 * Reserved, must be zero.
 *
 * @param[out] DriverRegKey
 * Receives the key handle.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
NTAPI
IoOpenDriverRegistryKey(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ DRIVER_REGKEY_TYPE RegKeyType,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Flags,
    _Out_ PHANDLE DriverRegKey)
{
    HANDLE ServiceKey;
    NTSTATUS Status;

    PAGED_CODE();

    if (!IopIsDriverObjectUsable(DriverObject) || Flags || !DriverRegKey)
        return STATUS_INVALID_PARAMETER;

    Status = IopOpenServiceKey(DriverObject, KEY_READ | KEY_CREATE_SUB_KEY, &ServiceKey);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IopCheckServiceIsDriver(ServiceKey);
    if (!NT_SUCCESS(Status))
        goto Exit;

    switch (RegKeyType)
    {
        case DriverRegKeyParameters:
        {
            if (DesiredAccess & MAXIMUM_ALLOWED)
                DesiredAccess = (DesiredAccess & ~MAXIMUM_ALLOWED) | KEY_READ;

            /* Drivers only ever read their parameters */
            if (DesiredAccess & ~(KEY_READ | GENERIC_READ))
            {
                Status = STATUS_ACCESS_DENIED;
                break;
            }

            Status = IopOpenRegistryKeyEx(DriverRegKey, ServiceKey, &IopParametersName, DesiredAccess);
            break;
        }

        case DriverRegKeyPersistentState:
        {
            Status = IopCreateRegistryKeyEx(DriverRegKey,
                                            ServiceKey,
                                            &IopStateName,
                                            DesiredAccess,
                                            REG_OPTION_NON_VOLATILE,
                                            NULL);
            break;
        }

        case DriverRegKeySharedPersistentState:
        {
            Status = IopCreateRegistryKeyEx(DriverRegKey,
                                            ServiceKey,
                                            &IopSharedStateName,
                                            DesiredAccess,
                                            REG_OPTION_NON_VOLATILE,
                                            NULL);
            break;
        }

        default:
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

Exit:
    ZwClose(ServiceKey);
    return Status;
}

/**
 * @brief
 * Opens the directory a driver was loaded from, or one it keeps state in.
 *
 * @param[in] DriverObject
 * The driver.
 *
 * @param[in] DirectoryType
 * Image opens the image directory. Data and SharedData live under
 * \SystemRoot\ServiceState\<service> and are created on first use.
 *
 * @param[in] Flags
 * Reserved, must be zero.
 *
 * @param[out] DriverDirectoryHandle
 * Receives the directory handle.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
NTAPI
IoGetDriverDirectory(
    _In_ PVOID DriverObject,
    _In_ DRIVER_DIRECTORY_TYPE DirectoryType,
    _In_ ULONG Flags,
    _Out_ PHANDLE DriverDirectoryHandle)
{
    PDRIVER_OBJECT Driver = DriverObject;
    PCUNICODE_STRING Components[3];
    UNICODE_STRING Path;
    NTSTATUS Status;

    PAGED_CODE();

    if (!IopIsDriverObjectUsable(Driver) || Flags || !DriverDirectoryHandle)
        return STATUS_INVALID_PARAMETER;

    switch (DirectoryType)
    {
        case DriverDirectoryImage:
            return IopGetDriverImageDirectory(Driver, DriverDirectoryHandle);

        case DriverDirectoryData:
            Components[2] = &IopDataName;
            break;

        case DriverDirectorySharedData:
            Components[2] = &IopSharedDataName;
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    Components[0] = &IopServiceStateRoot;
    Components[1] = &Driver->DriverExtension->ServiceKeyName;

    Status = IopJoinPath(&Path, Components, RTL_NUMBER_OF(Components));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IopOpenStateDirectory(&Path, TRUE, DriverDirectoryHandle);
    ExFreePoolWithTag(Path.Buffer, TAG_IO);
    return Status;
}

/**
 * @brief
 * Opens the directory a device keeps state in, creating it on first use.
 *
 * @param[in] PhysicalDeviceObject
 * The device.
 *
 * @param[in] DirectoryType
 * Only DeviceDirectoryData exists.
 *
 * @param[in] Flags
 * Reserved, must be zero.
 *
 * @param[in] Reserved
 * Must be NULL.
 *
 * @param[out] DeviceDirectoryHandle
 * Receives the directory handle.
 *
 * @remarks
 * The directory is \SystemRoot\System32\DriverState\Devices\<name>\Data. The
 * name is the instance path with unusable characters turned into '#', or a new
 * GUID when no directory by that name exists yet. It is kept as a device
 * property so later calls find the same directory.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS
NTAPI
IoGetDeviceDirectory(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ DEVICE_DIRECTORY_TYPE DirectoryType,
    _In_ ULONG Flags,
    _In_ PVOID Reserved,
    _Out_ PHANDLE DeviceDirectoryHandle)
{
    UNICODE_STRING GuidName = { 0 };
    UNICODE_STRING Name = { 0 };
    PUNICODE_STRING UsedName;
    PDEVICE_NODE DeviceNode;
    HANDLE Directory = NULL;
    NTSTATUS Status;
    GUID Guid;

    PAGED_CODE();

    if (!PhysicalDeviceObject || Flags || Reserved || !DeviceDirectoryHandle)
        return STATUS_INVALID_PARAMETER;

    DeviceNode = IopGetDeviceNode(PhysicalDeviceObject);
    if (!DeviceNode || !DeviceNode->InstancePath.Length || !DeviceNode->InstancePath.Buffer)
        return STATUS_INVALID_PARAMETER;

    if (DirectoryType != DeviceDirectoryData)
        return STATUS_INVALID_PARAMETER;

    Status = IopQueryDeviceStateName(PhysicalDeviceObject, &Name);
    if (NT_SUCCESS(Status))
    {
        Status = IopOpenDeviceStateDirectory(&Name, TRUE, DeviceDirectoryHandle);
        ExFreePoolWithTag(Name.Buffer, TAG_IO);
        return Status;
    }

    if (Status != STATUS_OBJECT_NAME_NOT_FOUND)
        return Status;

    Status = IopMakeDeviceStateName(&DeviceNode->InstancePath, &Name);
    if (!NT_SUCCESS(Status))
        return Status;

    UsedName = &Name;
    Status = IopOpenDeviceStateDirectory(&Name, FALSE, &Directory);
    if (Status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        Status = ExUuidCreate(&Guid);
        if (!NT_SUCCESS(Status))
            goto Exit;

        Status = RtlStringFromGUID(&Guid, &GuidName);
        if (!NT_SUCCESS(Status))
            goto Exit;

        UsedName = &GuidName;
        Status = IopOpenDeviceStateDirectory(&GuidName, TRUE, &Directory);
    }

    if (!NT_SUCCESS(Status))
        goto Exit;

    /* RtlStringFromGUID and IopMakeDeviceStateName both terminate the name */
    Status = IoSetDevicePropertyData(PhysicalDeviceObject,
                                     &IopStateDirectoryKey,
                                     LOCALE_NEUTRAL,
                                     0,
                                     DEVPROP_TYPE_STRING,
                                     UsedName->Length + sizeof(UNICODE_NULL),
                                     UsedName->Buffer);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(Directory);
        goto Exit;
    }

    *DeviceDirectoryHandle = Directory;

Exit:
    RtlFreeUnicodeString(&GuidName);
    RtlFreeUnicodeString(&Name);
    return Status;
}

/* EOF */
