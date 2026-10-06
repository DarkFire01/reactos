/*
 * PROJECT:     ReactOS ACPI Platform Extensions
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub private object model
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * ACPI gives a peripheral a CmResourceTypeConnection descriptor carrying a 64
 * bit connection id instead of the bus address its firmware declared. The hub
 * formats that id into \Device\RESOURCE_HUB\<16 hex digits> and answers the open
 * with STATUS_REPARSE, so the create lands on the controller that owns it.
 */

#pragma once

#include <ntifs.h>
#include <ntddk.h>
#include <wdf.h>
/* The hub both parses and formats connection ids */
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <drivers/acpi/restrans.h>

#define RH_POOL_TAG 'HseR'

/*
 * A GPIO interrupt is cached under CLASS_GPIO with this type. It never reaches a
 * public resource descriptor, so unlike the CM_RESOURCE_CONNECTION_TYPE_* values
 * in <wdm.h> this one is only ever a cache key.
 */
#define CM_RESOURCE_CONNECTION_TYPE_GPIO_INTERRUPT 0x01

/*
 * The controller a provider is connected to. Split from the provider because a
 * provider survives its controller going away and coming back.
 */
typedef struct _RH_TARGET_DATA
{
    LIST_ENTRY Link;
    UNICODE_STRING ControllerName;
    WDFIOTARGET IoTarget;
    volatile LONG ReferenceCount;
} RH_TARGET_DATA, *PRH_TARGET_DATA;

/* A bus controller that produces connections, one per ACPI ResourceSource name */
typedef struct _RH_PROVIDER
{
    LIST_ENTRY Link;
    LIST_ENTRY ConnectionList;
    LIST_ENTRY TargetList;
    UNICODE_STRING BiosName;
    PDEVICE_OBJECT DeviceObject;
    PRH_TARGET_DATA TargetData;
    volatile LONG ReferenceCount;
} RH_PROVIDER, *PRH_PROVIDER;

/*
 * One ACPI Connection() descriptor. ConnectionProperties holds the raw firmware
 * bytes that IOCTL_RH_QUERY_CONNECTION_PROPERTIES hands back verbatim.
 */
typedef struct _RH_CONNECTION
{
    LIST_ENTRY Link;
    LIST_ENTRY ProviderLink;
    LIST_ENTRY ContextList;
    PRH_PROVIDER Provider;
    LARGE_INTEGER Id;
    /* CM_RESOURCE_CONNECTION_CLASS_* */
    UCHAR Class;
    /* CM_RESOURCE_CONNECTION_TYPE_*, scoped by Class */
    UCHAR Type;
    PVOID ConnectionProperties;
    ULONG PropertiesLength;
    /* GPIO interrupts only */
    ULONG InterruptVector;
    volatile LONG ReferenceCount;
} RH_CONNECTION, *PRH_CONNECTION;

/* Context on the hub's WDFDEVICE */
typedef struct _RH_DEVICE_CONTEXT
{
    WDFDEVICE Device;

    /*
     * One lock over both lists, because every path that touches a connection
     * also walks to its provider.
     */
    KSPIN_LOCK Lock;
    LIST_ENTRY ConnectionList;
    LIST_ENTRY ProviderList;
} RH_DEVICE_CONTEXT, *PRH_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RH_DEVICE_CONTEXT, RhGetDeviceContext)


/* connection.c */

NTSTATUS
RhpAllocateConnection(
    _Outptr_ PRH_CONNECTION *Connection);

VOID
RhpFreeConnection(
    _In_ PRH_CONNECTION Connection);

PRH_CONNECTION
RhpFindAndReferenceConnectionById(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ LARGE_INTEGER Id);

PRH_CONNECTION
RhpFindAndReferenceConnectionByVector(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ ULONG InterruptVector);

VOID
RhpDereferenceConnection(
    _In_ PRH_CONNECTION Connection);

NTSTATUS
RhpInsertConnectionLocked(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PRH_CONNECTION Connection);

PRH_CONNECTION
RhpFindConnectionLocked(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PRH_PROVIDER Provider,
    _In_ UCHAR Class,
    _In_ UCHAR Type,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength);

/* provider.c */

NTSTATUS
RhpAllocateProvider(
    _Outptr_ PRH_PROVIDER *Provider);

VOID
RhpFreeProvider(
    _In_ PRH_PROVIDER Provider);

VOID
RhpDereferenceProvider(
    _In_ PRH_PROVIDER Provider);

PRH_TARGET_DATA
RhpReferenceProviderTargetData(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ PRH_PROVIDER Provider);

VOID
RhpDereferenceTargetData(
    _In_ PRH_TARGET_DATA TargetData);

/* create.c */

EVT_WDFDEVICE_WDM_IRP_PREPROCESS RhWdmDeviceFileCreate;

NTSTATUS
RhpBuildReparseName(
    _In_ WDFDEVICE Device,
    _In_ PCUNICODE_STRING FileName,
    _Out_ PUNICODE_STRING NewFileName);

VOID
RhpFreeUnicodeString(
    _Inout_ PUNICODE_STRING String);

/* translate.c */

NTSTATUS
RhpProcessTranslationInterfaceQueryIoctl(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST Request,
    _Out_ PULONG_PTR Information);

/* ioctl.c */

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RhEvtProcessDeviceIoControl;
