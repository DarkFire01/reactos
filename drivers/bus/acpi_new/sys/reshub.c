/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource hub client for ACPI connection descriptors
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

#include "reshub_downlevel.h"
#include <ndk/haltypes.h>   // HAL_PRIVATE_DISPATCH, as the kernel declares it
#include <drivers/acpi/restrans.h>

// Hub interface and the fast mutex guarding it. All callers run <= APC_LEVEL.
static RESOURCE_TRANSLATION_INTERFACE_STANDARD UacpiTranslationInterface;
static BOOLEAN UacpiTranslationInterfaceValid = FALSE;
static FAST_MUTEX UacpiTranslationInterfaceLock;

// Forward to the HAL's secondary GSIV allocator. The hub passes a NULL name.
static
NTSTATUS
NTAPI
UacpiAllocateGsivForSecondaryInterrupt(
    _In_ PCHAR DescriptorName,
    _In_ ULONG DescriptorNameLength,
    _Out_ PULONG Gsiv)
{
    NTSTATUS Status;

#if (NTDDI_VERSION >= NTDDI_WIN8) || defined(__REACTOS__)
    if (HALPRIVATEDISPATCH->HalAllocateGsivForSecondaryInterrupt == NULL)
    {
        // No allocator: a GSIV minted here could never be connected.
        UacpiTrace("[acpi] reshub: HAL has no secondary GSIV allocator\n");
        return STATUS_NOT_SUPPORTED;
    }

    Status = HALPRIVATEDISPATCH->HalAllocateGsivForSecondaryInterrupt(
                 (PCCHAR)DescriptorName,
                 (USHORT)DescriptorNameLength,
                 Gsiv);
    if (!NT_SUCCESS(Status))
    {
        UacpiTrace("[acpi] reshub: secondary GSIV allocation failed 0x%08lx\n", Status);
        return Status;
    }

    UacpiTrace("[acpi] reshub: secondary GSIV %u allocated\n", *Gsiv);
    return STATUS_SUCCESS;
#else
    // No secondary GSIV allocator before Win8.
    UNREFERENCED_PARAMETER(DescriptorName);
    UNREFERENCED_PARAMETER(DescriptorNameLength);
    UNREFERENCED_PARAMETER(Gsiv);
    UNREFERENCED_PARAMETER(Status);
    return STATUS_NOT_SUPPORTED;
#endif
}

// Namespace node for one of our device objects, or NULL.
static
uacpi_namespace_node *
UacpiNodeFromDeviceObject(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PUACPI_COMMON Common;

    if (DeviceObject == NULL || DeviceObject->DeviceExtension == NULL)
    {
        return NULL;
    }

    Common = (PUACPI_COMMON)DeviceObject->DeviceExtension;
    if (Common->Self != DeviceObject)
    {
        return NULL;
    }

    switch (Common->Type)
    {
        case UacpiExtPdo:
            return ((PUACPI_PDO)Common)->Node;

        case UacpiExtFilter:
            return ((PUACPI_FILTER)Common)->Node;

        default:
            return NULL;
    }
}

// Record the GpioInt trigger mode from the hub in the irqlib GSIV tables.
static
NTSTATUS
NTAPI
UacpiUpdateInterruptProperties(
    _In_ ULONG Gsiv,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity)
{
    if (Mode == Latched)
    {
        UacpiIrqLibNoteEdgeGsiv(Gsiv);
    }
    else
    {
        UacpiIrqLibNoteLevelGsiv(Gsiv);
    }

    UacpiTrace("[acpi] reshub: GSIV %u mode %s polarity %u\n",
               Gsiv, (Mode == Latched) ? "edge" : "level", (ULONG)Polarity);

    return STATUS_SUCCESS;
}

// Resolve ResourceSource in the device's scope; return the absolute path.
static
NTSTATUS
NTAPI
UacpiGetFullyQualifiedBiosName(
    _In_ PDEVICE_OBJECT BiosDeviceObject,
    _In_ PSTRING BiosName,
    _Out_ PUNICODE_STRING FullyQualifiedBiosName,
    _Out_ PULONG StringLength)
{
    uacpi_namespace_node *Scope = NULL;
    uacpi_namespace_node *Target = NULL;
    const uacpi_char *AbsolutePath;
    ANSI_STRING PathString;
    USHORT Required;
    NTSTATUS Status;

    *StringLength = 0;

    if (BiosName == NULL || BiosName->Buffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Scope is the requesting device, else the root.
    if (BiosDeviceObject != NULL)
    {
        Scope = UacpiNodeFromDeviceObject(BiosDeviceObject);
    }
    if (Scope == NULL)
    {
        Scope = uacpi_namespace_root();
    }

    if (uacpi_unlikely_error(
            uacpi_namespace_node_find(Scope, BiosName->Buffer, &Target)) ||
        Target == NULL)
    {
        UacpiTrace("[acpi] reshub: cannot resolve ResourceSource '%s'\n",
                   BiosName->Buffer);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    AbsolutePath = uacpi_namespace_node_generate_absolute_path(Target);
    if (AbsolutePath == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitAnsiString(&PathString, AbsolutePath);

    // Report the required size; the hub retries once with a larger buffer.
    Required = (USHORT)RtlAnsiStringToUnicodeSize(&PathString);
    *StringLength = Required;

    if (FullyQualifiedBiosName->MaximumLength < Required)
    {
        uacpi_free_absolute_path(AbsolutePath);
        return STATUS_BUFFER_TOO_SMALL;
    }

    Status = RtlAnsiStringToUnicodeString(FullyQualifiedBiosName, &PathString, FALSE);
    uacpi_free_absolute_path(AbsolutePath);

    return Status;
}

static
VOID
NTAPI
UacpiUnloadTranslationInterface(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    ExAcquireFastMutex(&UacpiTranslationInterfaceLock);
    UacpiTranslationInterfaceValid = FALSE;
    RtlZeroMemory(&UacpiTranslationInterface, sizeof(UacpiTranslationInterface));
    ExReleaseFastMutex(&UacpiTranslationInterfaceLock);
}

// Load the hub if needed, open it, and exchange interfaces over one IOCTL.
NTSTATUS
UacpiConnectResourceHub(VOID)
{
    DECLARE_CONST_UNICODE_STRING(HubDeviceName, RESOURCE_HUB_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(HubServiceName,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\acpiex");
    RESOURCE_TRANSLATION_INTERFACE_STANDARD Interface;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    PDEVICE_OBJECT HubDevice = NULL;
    PFILE_OBJECT HubFile = NULL;
    HANDLE HubHandle = NULL;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    PAGED_CODE();

    ExInitializeFastMutex(&UacpiTranslationInterfaceLock);

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)&HubDeviceName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&HubHandle,
                        GENERIC_READ | GENERIC_WRITE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NON_DIRECTORY_FILE);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        // Demand start: nothing has needed the hub until now.
        Status = ZwLoadDriver((PUNICODE_STRING)&HubServiceName);
        if (!NT_SUCCESS(Status) && Status != STATUS_IMAGE_ALREADY_LOADED)
        {
            UacpiTrace("[acpi] reshub: ZwLoadDriver(acpiex) failed 0x%X\n", Status);
        }

        Status = ZwOpenFile(&HubHandle,
                            GENERIC_READ | GENERIC_WRITE,
                            &ObjectAttributes,
                            &IoStatusBlock,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            FILE_NON_DIRECTORY_FILE);
    }

    if (!NT_SUCCESS(Status))
    {
        UacpiTrace("[acpi] reshub: cannot open %wZ (0x%X) - connection "
                   "resources will not be translated\n",
                   &HubDeviceName, Status);
        return Status;
    }

    Status = ObReferenceObjectByHandle(HubHandle,
                                       GENERIC_READ | GENERIC_WRITE,
                                       *IoFileObjectType,
                                       KernelMode,
                                       (PVOID *)&HubFile,
                                       NULL);
    ZwClose(HubHandle);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    HubDevice = IoGetAttachedDeviceReference(HubFile->DeviceObject);

    RtlZeroMemory(&Interface, sizeof(Interface));
    Interface.Interface.Size = sizeof(Interface);
    Interface.Interface.Version = RESOURCE_TRANSLATION_INTERFACE_VERSION;
    Interface.AcpiContext = NULL;
    Interface.UnloadTranslationInterface = UacpiUnloadTranslationInterface;
    Interface.GetFullyQualifiedBiosName = UacpiGetFullyQualifiedBiosName;
    Interface.AllocateGsivForSecondaryInterrupt = UacpiAllocateGsivForSecondaryInterrupt;
    Interface.UpdateInterruptProperties = UacpiUpdateInterruptProperties;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IOCTL_RH_QUERY_TRANSLATION_INTERFACE,
                                        HubDevice,
                                        &Interface,
                                        sizeof(Interface),
                                        &Interface,
                                        sizeof(Interface),
                                        FALSE,
                                        &Event,
                                        &IoStatusBlock);
    if (Irp == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Status = IoCallDriver(HubDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock.Status;
    }

    if (!NT_SUCCESS(Status))
    {
        UacpiTrace("[acpi] reshub: translator interface query failed 0x%X\n", Status);
        goto Cleanup;
    }

    // Validate what came back before trusting a function pointer from it.
    if (Interface.Interface.Version != RESOURCE_TRANSLATION_INTERFACE_VERSION ||
        Interface.TranslateBiosToNtResources == NULL ||
        Interface.BiosNameToDeviceCallback == NULL)
    {
        UacpiTrace("[acpi] reshub: hub returned an unusable interface\n");
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }

    ExAcquireFastMutex(&UacpiTranslationInterfaceLock);
    UacpiTranslationInterface = Interface;
    UacpiTranslationInterfaceValid = TRUE;
    ExReleaseFastMutex(&UacpiTranslationInterfaceLock);

    UacpiTrace("[acpi] reshub: translator interface bound\n");
    Status = STATUS_SUCCESS;

Cleanup:
    if (HubDevice != NULL)
    {
        ObDereferenceObject(HubDevice);
    }
    ObDereferenceObject(HubFile);

    return Status;
}

// Tell the hub which device object owns a firmware name.
NTSTATUS
UacpiAddBiosNameDeviceAssociation(
    _In_ PCUNICODE_STRING BiosName,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    ExAcquireFastMutex(&UacpiTranslationInterfaceLock);

    if (UacpiTranslationInterfaceValid)
    {
        Status = UacpiTranslationInterface.BiosNameToDeviceCallback(
                     UacpiTranslationInterface.Context, BiosName, DeviceObject);
    }

    ExReleaseFastMutex(&UacpiTranslationInterfaceLock);

    return Status;
}

// Translate one connection descriptor; retry at most once on a size request.
NTSTATUS
UacpiTranslateConnectionDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    PRESOURCE_TRANSLATION_RESULT Result = NULL;
    ULONG Size = 120;
    ULONG Attempt;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    ExAcquireFastMutex(&UacpiTranslationInterfaceLock);

    if (!UacpiTranslationInterfaceValid)
    {
        ExReleaseFastMutex(&UacpiTranslationInterfaceLock);
        return STATUS_NOT_SUPPORTED;
    }

    for (Attempt = 0; Attempt < 2; Attempt++)
    {
        Result = (PRESOURCE_TRANSLATION_RESULT)
                     ExAllocatePoolWithTag(PagedPool, Size, UACPI_POOL_TAG);
        if (Result == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        Status = UacpiTranslationInterface.TranslateBiosToNtResources(
                     UacpiTranslationInterface.Context,
                     DeviceObject,
                     Descriptor,
                     DescriptorLength,
                     0,
                     Result,
                     &Size);

        if (NT_SUCCESS(Status))
        {
            *IoDescriptor = Result->Descriptor;
        }

        ExFreePoolWithTag(Result, UACPI_POOL_TAG);
        Result = NULL;

        if (Status != STATUS_BUFFER_TOO_SMALL || Size == 0)
        {
            break;
        }
    }

    ExReleaseFastMutex(&UacpiTranslationInterfaceLock);

    return Status;
}

// Register every started PDO's path with the hub as a potential provider.
VOID
UacpiRegisterBiosNameForPdo(PUACPI_PDO Pdo)
{
    const uacpi_char *AbsolutePath;
    ANSI_STRING PathAnsi;
    UNICODE_STRING PathUnicode;
    NTSTATUS Status;

    PAGED_CODE();

    if (Pdo == NULL || Pdo->Node == NULL || Pdo->Common.Self == NULL)
    {
        return;
    }

    // Nothing to register against until the hub interface is bound.
    ExAcquireFastMutex(&UacpiTranslationInterfaceLock);
    Status = UacpiTranslationInterfaceValid ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    ExReleaseFastMutex(&UacpiTranslationInterfaceLock);

    if (!NT_SUCCESS(Status))
    {
        return;
    }

    AbsolutePath = uacpi_namespace_node_generate_absolute_path(Pdo->Node);
    if (AbsolutePath == NULL)
    {
        return;
    }

    RtlInitAnsiString(&PathAnsi, AbsolutePath);

    Status = RtlAnsiStringToUnicodeString(&PathUnicode, &PathAnsi, TRUE);
    uacpi_free_absolute_path(AbsolutePath);

    if (!NT_SUCCESS(Status))
    {
        return;
    }

    Status = UacpiAddBiosNameDeviceAssociation(&PathUnicode, Pdo->Common.Self);
    if (NT_SUCCESS(Status))
    {
        UacpiTrace("[acpi] reshub: %wZ -> %p registered as a provider\n",
                   &PathUnicode, Pdo->Common.Self);
    }

    RtlFreeUnicodeString(&PathUnicode);
}
