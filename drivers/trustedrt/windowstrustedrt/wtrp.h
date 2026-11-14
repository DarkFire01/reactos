/*
 * PROJECT:     ReactOS Trusted Runtime class extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <wdfldr.h>
#include <initguid.h>
#include <wdmguid.h>
#include <drivers/trustedrt/wtrclass.h>

#define WTR_POOL_TAG                'EErT'

/* Deprecated alias of the eMMC RPMB interface some services still list */
#define WTR_DEPRECATED_RPMB_ID      GUID_WTR_RPMB_SERVICE

/* Stack given to client callbacks for each WTR_STACK_* choice */
#define WTR_STACK_SIZE_SMALL        0x900
#define WTR_STACK_SIZE_MEDIUM       0x1100
#define WTR_STACK_SIZE_LARGE        0x2100

#define WTR_LOG_STACK_SIZE          2048
#define WTR_LOG_BUFFER_SIZE         512

#ifndef SDDL_REVISION_1
#define SDDL_REVISION_1             1
#endif

NTKERNELAPI
NTSTATUS
NTAPI
KeExpandKernelStackAndCalloutEx(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ SIZE_T Size,
    _In_ BOOLEAN Wait,
    _In_opt_ PVOID Context);

NTKERNELAPI
NTSTATUS
NTAPI
ObSetSecurityObjectByPointer(
    _In_ PVOID Object,
    _In_ SECURITY_INFORMATION SecurityInformation,
    _In_ PSECURITY_DESCRIPTOR SecurityDescriptor);

/* Client callbacks run on a stack of the size the client asked for */
#define WtrExpandStack(Callout, Parameter, Size) \
    KeExpandKernelStackAndCalloutEx((Callout), (Parameter), (Size), TRUE, NULL)

/* Life of an IOCTL on a service session */
typedef enum _WTR_REQUEST_STATE
{
    WtrRequestNew = 0,
    WtrRequestExecuting,
    WtrRequestQueryCancelable,
    WtrRequestCancelable,
    WtrRequestCancelPending,
    WtrRequestCanceled,
    WtrRequestCompleted
} WTR_REQUEST_STATE;

/* Bits of WTR_CALL_DATA.Flags */
#define WTR_CALL_WAITER             0x1
#define WTR_CALL_COMPLETED          0x2

/* Bits of WTR_PLUGIN_REQUEST_CONTEXT.Flags */
#define WTR_PLUGIN_REQUEST_SENT     0x1
#define WTR_PLUGIN_REQUEST_DONE     0x2
#define WTR_PLUGIN_REQUEST_CANCEL   0x4

typedef VOID NTAPI WTR_CALL_COMPLETION(_In_ WDFOBJECT CallData);

/* One call into an OS service on behalf of the secure environment */
typedef struct _WTR_CALL_DATA
{
    volatile LONG Flags;
    NTSTATUS Status;
    ULONG64 Information;
    WTR_SERVICE_REQUEST Request;
    WTR_CALL_COMPLETION *Completion;
    PKEVENT CompletionEvent;
} WTR_CALL_DATA, *PWTR_CALL_DATA;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_CALL_DATA, WtrGetCallData);

typedef struct _WTR_COMPLETION_ITEM
{
    WDFOBJECT CallData;
} WTR_COMPLETION_ITEM, *PWTR_COMPLETION_ITEM;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_COMPLETION_ITEM, WtrGetCompletionItem);

typedef struct _WTR_BIND_CONTEXT
{
    ULONG MajorVersion;
    ULONG MinorVersion;
} WTR_BIND_CONTEXT, *PWTR_BIND_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_BIND_CONTEXT, WtrGetBindContext);

typedef struct _WTR_MASTER_CONTEXT
{
    BOOLEAN ServicesConfigured;
    ULONG Mode;
    ULONG StackSize;
    WDFCOLLECTION Services;
    WTR_CLIENT Client;
    WDFIOTARGET SelfTarget;
    WDFQUEUE SerializeQueue;
    WTR_DEVICE_CALLBACKS Callbacks;
    volatile LONG IdleHeld;
} WTR_MASTER_CONTEXT, *PWTR_MASTER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_MASTER_CONTEXT, WtrGetMasterContext);

typedef struct _WTR_MASTER_FILE_CONTEXT
{
    WDFQUEUE Queue;
} WTR_MASTER_FILE_CONTEXT, *PWTR_MASTER_FILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_MASTER_FILE_CONTEXT, WtrGetMasterFileContext);

/* One secure service the client reported, followed by its descriptor */
typedef struct _WTR_SERVICE_ENTRY
{
    WDFDEVICE ServiceDevice;
    WDFDEVICE Pdo;
    WTR_SERVICE_DESCRIPTOR Descriptor;
} WTR_SERVICE_ENTRY, *PWTR_SERVICE_ENTRY;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_SERVICE_ENTRY, WtrGetServiceEntry);

/* Child list identification of a service PDO */
typedef struct _WTR_CHILD_ID
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    GUID ServiceId;
} WTR_CHILD_ID, *PWTR_CHILD_ID;

typedef struct _WTR_SERVICE_CONTEXT
{
    PWTR_SERVICE_DESCRIPTOR Descriptor;
    WDFDEVICE MasterDevice;
    KEVENT CallDataFree;
    WDFOBJECT CallData;
    WDFSPINLOCK Lock;
    WDFCOLLECTION SessionQueues;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    WDFREQUEST ReservedRequest;
    GUID ServiceId;
    PWTR_MASTER_CONTEXT Master;
    WTR_SERVICE_CALLBACKS Callbacks;
    volatile LONG IdleHeld;
} WTR_SERVICE_CONTEXT, *PWTR_SERVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_SERVICE_CONTEXT, WtrGetServiceContext);

typedef struct _WTR_SESSION_CONTEXT
{
    PWTR_SERVICE_CONTEXT Service;
    WDFQUEUE Queue;
    PVOID ClientContext;
    volatile LONG Busy;
    WDFSPINLOCK Lock;
} WTR_SESSION_CONTEXT, *PWTR_SESSION_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_SESSION_CONTEXT, WtrGetSessionContext);

typedef struct _WTR_REQUEST_CONTEXT
{
    ULONG Priority;
    WDFOBJECT CallData;
    WDFMEMORY InputMemory;
    WDFMEMORY OutputMemory;
    WTR_SERVICE_REQUEST Request;
    PVOID CancelContext;
    WDFREQUEST Forwarded;
    NTSTATUS Status;
    ULONG64 Information;
    WTR_REQUEST_STATE State;
} WTR_REQUEST_CONTEXT, *PWTR_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_REQUEST_CONTEXT, WtrGetRequestContext);

/* OS services the secure environment can call, reached through device interfaces */
typedef struct _WTR_OS_SERVICES
{
    BOOLEAN Powered;
    WDFWAITLOCK Lock;
    WDFCOLLECTION Plugins;
} WTR_OS_SERVICES, *PWTR_OS_SERVICES;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_OS_SERVICES, WtrGetOsServices);

typedef NTSTATUS NTAPI WTR_PLUGIN_INITIALIZE(_In_ WDFOBJECT Plugin, _In_ WDFIOTARGET Target, _Out_ PWTR_OS_SERVICE_INFORMATION Information);
typedef NTSTATUS NTAPI WTR_PLUGIN_HANDLER(_In_ WDFOBJECT Plugin, _In_ WDFIOTARGET Target, _In_ WDFREQUEST Request, _In_ WDFOBJECT CallData);

typedef struct _WTR_PLUGIN
{
    WDFSTRING SymbolicLink;
    GUID Id;
    WTR_OS_SERVICE_INFORMATION Information;
    PVOID Notification;
    BOOLEAN Available;
    ULONG Generation;
    WDFWORKITEM WorkItem;
    WDFSPINLOCK Lock;
    WDFCOLLECTION FreeRequests;
    WDFCOLLECTION ActiveRequests;
    WDFCOLLECTION PendingCalls;
    WTR_PLUGIN_INITIALIZE *Initialize;
    WTR_PLUGIN_HANDLER *Handler;
} WTR_PLUGIN, *PWTR_PLUGIN;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_PLUGIN, WtrGetPlugin);

typedef struct _WTR_PLUGIN_REQUEST_CONTEXT
{
    WDFMEMORY Input;
    WDFMEMORY Output;
    WDFDEVICE Device;
    WDFOBJECT CallData;
    PWTR_PLUGIN Plugin;
    ULONG Generation;
    WDFIOTARGET Target;
    ULONG SendCount;
    volatile LONG Flags;
} WTR_PLUGIN_REQUEST_CONTEXT, *PWTR_PLUGIN_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_PLUGIN_REQUEST_CONTEXT, WtrGetPluginRequest);

typedef struct _WTR_PLUGIN_WORK
{
    WDFOBJECT Plugin;
} WTR_PLUGIN_WORK, *PWTR_PLUGIN_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_PLUGIN_WORK, WtrGetPluginWork);

typedef struct _WTR_RPMB_CONTEXT
{
    ULONG MaxSectors;
    WDFMEMORY FrameMemory;
    PVOID FrameBuffer;
    BOOLEAN UseStorageProperty;
} WTR_RPMB_CONTEXT, *PWTR_RPMB_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WTR_RPMB_CONTEXT, WtrGetRpmbContext);

/* driver.c */
extern WDFCOLLECTION WtrSecureServices;

/* master.c */
NTSTATUS
NTAPI
WtrAddMasterDevice(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ ULONG Flags,
    _Out_ WDFDEVICE *Device);

NTSTATUS
NTAPI
WtrReportServiceDevice(
    _In_ WDFDEVICE ServiceDevice,
    _Out_ WDFDEVICE *MasterDevice,
    _Out_ PWTR_SERVICE_DESCRIPTOR *Descriptor);

VOID
NTAPI
WtrEnumerateServiceIds(
    _In_ WDFREQUEST Request);

/* service.c */
NTSTATUS
NTAPI
WtrAddServiceDevice(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ const GUID *ServiceId,
    _Out_ WDFDEVICE *ServiceDevice,
    _Out_ WDFDEVICE *MasterDevice);

NTSTATUS
NTAPI
WtrCompleteSessionRequest(
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information);

NTSTATUS
NTAPI
WtrExecuteSessionRequest(
    _In_ WDFREQUEST Request);

NTSTATUS
NTAPI
WtrCallOsService(
    _In_ WDFDEVICE ServiceDevice,
    _In_ const GUID *OsServiceId,
    _In_ PWTR_SERVICE_REQUEST Request,
    _Out_opt_ PULONG64 Information);

VOID
NTAPI
WtrFinishCall(
    _In_ WDFDEVICE Device,
    _In_ WDFOBJECT CallData,
    _In_ NTSTATUS Status,
    _In_ ULONG64 Information);

/* osservice.c */
NTSTATUS
NTAPI
WtrOsServicesInitialize(
    _In_ WDFDEVICE MasterDevice);

NTSTATUS
NTAPI
WtrOsServiceQuery(
    _In_ WDFDEVICE MasterDevice,
    _In_ const GUID *OsServiceId,
    _Out_ PWTR_OS_SERVICE_INFORMATION Information);

NTSTATUS
NTAPI
WtrOsServiceExecute(
    _In_ WDFDEVICE MasterDevice,
    _In_ const GUID *OsServiceId,
    _In_ WDFOBJECT CallData);

BOOLEAN
NTAPI
WtrFindPlugin(
    _In_ PWTR_OS_SERVICES Services,
    _In_opt_ PCUNICODE_STRING SymbolicLink,
    _In_opt_ const GUID *Id,
    _Out_ WDFOBJECT *Plugin,
    _Out_opt_ PWTR_PLUGIN *PluginContext);

/* rpmb.c */
WTR_PLUGIN_INITIALIZE WtrRpmbInitialize;
WTR_PLUGIN_HANDLER WtrRpmbHandler;

DRIVER_NOTIFICATION_CALLBACK_ROUTINE WtrRpmbNotification;
