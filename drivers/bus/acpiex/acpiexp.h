/*
 * PROJECT:     ReactOS ACPI extension driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource hub internals
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntifs.h>
#include <ntstrsafe.h>
#include <reshub.h>
#include <drivers/acpi/restrans.h>

#define RH_POOL_TAG 'xEhR'

/* One translated GpioInt, GpioIo, SerialBus or FunctionConfig descriptor */
typedef struct _RH_CONNECTION
{
    LIST_ENTRY Link;
    LARGE_INTEGER Id;

    /* The secondary GSIV a GpioInt was given, zero for every other kind */
    ULONG Gsiv;
    UCHAR InitialPolarity;

    PDEVICE_OBJECT Consumer;
    UNICODE_STRING Provider;
    IO_RESOURCE_DESCRIPTOR Translated;

    ULONG DescriptorLength;
    UCHAR Descriptor[ANYSIZE_ARRAY];
} RH_CONNECTION, *PRH_CONNECTION;

/* A device ACPI knows by a namespace path, the target a connection opens to */
typedef struct _RH_PROVIDER
{
    LIST_ENTRY Link;
    PDEVICE_OBJECT DeviceObject;
    UNICODE_STRING BiosName;
} RH_PROVIDER, *PRH_PROVIDER;

/* translate.c */

VOID
NTAPI
RhInitializeTranslation(VOID);

NTSTATUS
NTAPI
RhExchangeTranslationInterface(
    _Inout_ PUACPINT_RH_TRANSLATION_INTERFACE Interface,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information);

NTSTATUS
NTAPI
RhQueryConnectionProperties(
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input,
    _Out_writes_bytes_(OutputLength) PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information);

NTSTATUS
NTAPI
RhQueryInitialPolarity(
    _In_ ULONG Gsiv,
    _Out_ PUCHAR InitialPolarity);

NTSTATUS
NTAPI
RhUpdateInitialPolarity(
    _In_ ULONG Gsiv,
    _In_ UCHAR InitialPolarity);

NTSTATUS
NTAPI
RhGetConnectionTarget(
    _In_ LARGE_INTEGER Id,
    _Out_ PUNICODE_STRING TargetName);
