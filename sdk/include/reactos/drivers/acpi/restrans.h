/*
 * PROJECT:     ReactOS ACPI
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource translation interface shared by ACPI and the Resource Hub
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * ACPI hands Connection() descriptors to the owner of \Device\RESOURCE_HUB
 * with one IOCTL. ACPI fills in its callbacks before sending it, and the hub
 * fills in its own before completing it.
 */

#pragma once

#define IOCTL_RH_QUERY_TRANSLATION_INTERFACE \
    CTL_CODE(FILE_DEVICE_BUS_EXTENDER,       \
             0xA,                            \
             METHOD_BUFFERED,                \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define RESOURCE_TRANSLATION_INTERFACE_VERSION 1

/*
 * Output of TranslateBiosToNtResources. When the buffer is too small the hub
 * returns STATUS_BUFFER_TOO_SMALL with the size it needs, and ACPI retries once.
 */
typedef struct _RESOURCE_TRANSLATION_RESULT
{
    ULONG Count;
    ULONG Reserved;
    IO_RESOURCE_DESCRIPTOR Descriptor;
    /* Offset from the start of this structure */
    ULONG BiosNameOffset;
    ULONG BiosNameLength;
    CHAR BiosName[ANYSIZE_ARRAY];
} RESOURCE_TRANSLATION_RESULT, *PRESOURCE_TRANSLATION_RESULT;

#define RESOURCE_TRANSLATION_RESULT_MIN_SIZE 56

/* ACPI to hub */

typedef VOID
(NTAPI *PACPI_UNLOAD_TRANSLATION_INTERFACE)(
    _In_ PVOID Context);

typedef NTSTATUS
(NTAPI *PACPI_GET_FULLY_QUALIFIED_BIOS_NAME)(
    _In_ PDEVICE_OBJECT BiosDeviceObject,
    _In_ PSTRING BiosName,
    _Out_ PUNICODE_STRING FullyQualifiedBiosName,
    _Out_ PULONG StringLength);

typedef NTSTATUS
(NTAPI *PACPI_ALLOCATE_GSIV_FOR_SECONDARY_INTERRUPT)(
    _In_ PCHAR DescriptorName,
    _In_ ULONG DescriptorNameLength,
    _Out_ PULONG Gsiv);

typedef NTSTATUS
(NTAPI *PACPI_UPDATE_INTERRUPT_PROPERTIES)(
    _In_ ULONG Gsiv,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity);

/* Hub to ACPI */

typedef NTSTATUS
(NTAPI *PRH_TRANSLATE_BIOS_TO_NT_RESOURCES)(
    _In_ PVOID Context,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _In_ ULONG Flags,
    _Out_ PRESOURCE_TRANSLATION_RESULT Result,
    _Inout_ PULONG SizeInOut);

typedef NTSTATUS
(NTAPI *PRH_BIOS_NAME_TO_DEVICE)(
    _In_ PVOID Context,
    _In_ PCUNICODE_STRING BiosName,
    _In_ PDEVICE_OBJECT DeviceObject);

typedef NTSTATUS
(NTAPI *PRH_QUERY_TRANSLATED_DESCRIPTOR_FOR_GSIV)(
    _In_ ULONG Gsiv,
    _In_ ULONG Flags,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor);

typedef struct _RESOURCE_TRANSLATION_INTERFACE_STANDARD
{
    INTERFACE Interface;

    /* Filled in by ACPI */
    PVOID AcpiContext;
    PACPI_UNLOAD_TRANSLATION_INTERFACE UnloadTranslationInterface;
    PACPI_GET_FULLY_QUALIFIED_BIOS_NAME GetFullyQualifiedBiosName;
    PACPI_ALLOCATE_GSIV_FOR_SECONDARY_INTERRUPT AllocateGsivForSecondaryInterrupt;
    PACPI_UPDATE_INTERRUPT_PROPERTIES UpdateInterruptProperties;

    /* Filled in by the hub */
    PVOID Context;
    PRH_TRANSLATE_BIOS_TO_NT_RESOURCES TranslateBiosToNtResources;
    PRH_BIOS_NAME_TO_DEVICE BiosNameToDeviceCallback;
    PRH_QUERY_TRANSLATED_DESCRIPTOR_FOR_GSIV QueryTranslatedDescriptorForGsiv;
} RESOURCE_TRANSLATION_INTERFACE_STANDARD, *PRESOURCE_TRANSLATION_INTERFACE_STANDARD;
