/*
 * PROJECT:     ReactOS Trusted Runtime class extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client interface of the WindowsTrustedRT KMDF class extension
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <wdf.h>

#define WTR_CLASS_NAME              L"WindowsTrustedRT"
#define WTR_CLASS_MAJOR_VERSION     1
#define WTR_CLASS_MINOR_VERSION     0

/* Bus the class enumerates one child per secure service on */
DEFINE_GUID(GUID_WTR_SECURE_SERVICE_BUS,
            0x4E815EE1, 0x20F8, 0x41EF, 0x8C, 0xFF, 0x3C, 0x28, 0x3F, 0x02, 0xD7, 0x22);

/* Interface the master publishes once an RPMB partition shows up */
DEFINE_GUID(GUID_WTR_RPMB_SERVICE,
            0x05272F96, 0xAD1A, 0x4497, 0x97, 0xC2, 0x16, 0xB9, 0x65, 0xB1, 0x97, 0xB3);

#ifndef GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB_DEFINED
#define GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB_DEFINED
DEFINE_GUID(GUID_DEVINTERFACE_EMMC_PARTITION_ACCESS_RPMB,
            0x27447C21, 0xBCC3, 0x4D07, 0xA0, 0x5B, 0xA3, 0x39, 0x5B, 0xB4, 0xEE, 0xE7);
#endif

/* Device type of every device the class creates */
#define FILE_DEVICE_WTR             0x56

#define IOCTL_WTR_QUERY_SERVICE     CTL_CODE(FILE_DEVICE_WTR, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WTR_ENUMERATE_SERVICES CTL_CODE(FILE_DEVICE_WTR, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WTR_SERIALIZE_REQUEST CTL_CODE(FILE_DEVICE_WTR, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WTR_EXECUTE_REQUEST   CTL_CODE(FILE_DEVICE_WTR, 1, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* Low two bits of WTR_DEVICE_CALLBACKS.Flags */
#define WTR_MODE_MASK               0x3
#define WTR_MODE_PARALLEL           0
#define WTR_MODE_SESSION_QUEUE      1
#define WTR_MODE_SEQUENTIAL         2
#define WTR_MODE_SERIALIZED         3

/* Next two bits pick how much stack client callbacks are given */
#define WTR_STACK_MASK              0xC
#define WTR_STACK_SMALL             0x4
#define WTR_STACK_MEDIUM            0x8
#define WTR_STACK_LARGE             0xC

/* Dependency kinds a service descriptor can list */
#define WTR_DEPENDENCY_OS_SERVICE   0
#define WTR_DEPENDENCY_DEVICE       1

typedef struct _WTR_SERVICE_DEPENDENCY
{
    ULONG Type;
    ULONG Reserved;
    GUID Id;
} WTR_SERVICE_DEPENDENCY, *PWTR_SERVICE_DEPENDENCY;

/* What a client reports for each secure service, followed by optional extension data */
typedef struct _WTR_SERVICE_DESCRIPTOR
{
    ULONG Size;
    GUID ServiceId;
    ULONG MajorVersion;
    ULONG MinorVersion;
    ULONG ExtensionOffset;
    ULONG DependencyCount;
    WTR_SERVICE_DEPENDENCY Dependencies[ANYSIZE_ARRAY];
} WTR_SERVICE_DESCRIPTOR, *PWTR_SERVICE_DESCRIPTOR;

/* Optional extension; SddlOffset is relative to the descriptor */
typedef struct _WTR_SERVICE_EXTENSION
{
    ULONG Size;
    ULONG SddlOffset;
} WTR_SERVICE_EXTENSION, *PWTR_SERVICE_EXTENSION;

/* Buffers of one execute request, as the caller passed them */
typedef struct _WTR_SERVICE_REQUEST
{
    ULONG Reserved[3];
    ULONG FunctionCode;
    PVOID InputBuffer;
    SIZE_T InputBufferLength;
    PVOID OutputBuffer;
    SIZE_T OutputBufferLength;
} WTR_SERVICE_REQUEST, *PWTR_SERVICE_REQUEST;

/* Reply of IOCTL_WTR_EXECUTE_REQUEST */
typedef struct _WTR_EXECUTE_RESULT
{
    ULONG Version;
    ULONG Reserved;
    ULONG64 Information;
} WTR_EXECUTE_RESULT, *PWTR_EXECUTE_RESULT;

/* Reply of IOCTL_WTR_QUERY_SERVICE */
typedef struct _WTR_SERVICE_VERSION
{
    ULONG Version;
    ULONG MajorVersion;
    ULONG MinorVersion;
} WTR_SERVICE_VERSION, *PWTR_SERVICE_VERSION;

/* Reply of QueryOSService */
typedef struct _WTR_OS_SERVICE_INFORMATION
{
    ULONG Version;
    ULONG MajorVersion;
    ULONG MinorVersion;
} WTR_OS_SERVICE_INFORMATION, *PWTR_OS_SERVICE_INFORMATION;

typedef NTSTATUS NTAPI WTR_EVT_DEVICE_ADDED(_In_ WDFDEVICE Device);
typedef NTSTATUS NTAPI WTR_EVT_DEVICE_DESTROY(_In_ WDFDEVICE Device);
typedef NTSTATUS NTAPI WTR_EVT_DEVICE_PREPARE_HARDWARE(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated);
typedef NTSTATUS NTAPI WTR_EVT_DEVICE_RELEASE_HARDWARE(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated);
typedef NTSTATUS NTAPI WTR_EVT_DEVICE_D0_ENTRY(_In_ WDFDEVICE Device);
typedef NTSTATUS NTAPI WTR_EVT_DEVICE_D0_EXIT(_In_ WDFDEVICE Device);
typedef NTSTATUS NTAPI WTR_EVT_DEVICE_QUERY_SERVICE(_In_ WDFDEVICE Device, _In_ ULONG Index, _Out_writes_bytes_opt_(*Size) PVOID Buffer, _Inout_ PULONG Size);
typedef VOID NTAPI WTR_EVT_DEVICE_OTHER_IO(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

typedef struct _WTR_SERVICE_CALLBACKS WTR_SERVICE_CALLBACKS, *PWTR_SERVICE_CALLBACKS;

typedef PWTR_SERVICE_CALLBACKS NTAPI WTR_EVT_DEVICE_GET_SERVICE_CALLBACKS(_In_ WDFDEVICE Device, _In_ const GUID *ServiceId);

typedef struct _WTR_DEVICE_CALLBACKS
{
    ULONG Flags;
    WTR_EVT_DEVICE_ADDED *EvtDeviceAdded;
    WTR_EVT_DEVICE_DESTROY *EvtDeviceDestroy;
    WTR_EVT_DEVICE_PREPARE_HARDWARE *EvtDevicePrepareHardware;
    WTR_EVT_DEVICE_RELEASE_HARDWARE *EvtDeviceReleaseHardware;
    WTR_EVT_DEVICE_D0_ENTRY *EvtDeviceD0Entry;
    WTR_EVT_DEVICE_D0_EXIT *EvtDeviceD0Exit;
    WTR_EVT_DEVICE_QUERY_SERVICE *EvtDeviceQueryService;
    WTR_EVT_DEVICE_OTHER_IO *EvtDeviceOtherIo;
    WTR_EVT_DEVICE_GET_SERVICE_CALLBACKS *EvtDeviceGetServiceCallbacks;
} WTR_DEVICE_CALLBACKS, *PWTR_DEVICE_CALLBACKS;

typedef NTSTATUS NTAPI WTR_EVT_SERVICE_ADDED(_In_ WDFDEVICE MasterDevice, _In_ const GUID *ServiceId, _In_ WDFDEVICE ServiceDevice);
typedef NTSTATUS NTAPI WTR_EVT_SERVICE_CLEANUP(_In_ WDFDEVICE ServiceDevice);
typedef NTSTATUS NTAPI WTR_EVT_SERVICE_CONNECT(_In_ WDFDEVICE ServiceDevice);
typedef NTSTATUS NTAPI WTR_EVT_SERVICE_DISCONNECT(_In_ WDFDEVICE ServiceDevice);
typedef NTSTATUS NTAPI WTR_EVT_SERVICE_CREATE_SESSION(_In_ WDFDEVICE ServiceDevice, _Out_ PVOID *SessionContext);
typedef NTSTATUS NTAPI WTR_EVT_SERVICE_DESTROY_SESSION(_In_ WDFDEVICE ServiceDevice, _Inout_ PVOID *SessionContext);
typedef NTSTATUS NTAPI WTR_EVT_SERVICE_PROCESS_REQUEST(
    _In_ WDFDEVICE ServiceDevice,
    _In_ PVOID SessionContext,
    _In_ WDFREQUEST Request,
    _In_ ULONG Priority,
    _In_ PWTR_SERVICE_REQUEST ServiceRequest,
    _In_ BOOLEAN UserMode,
    _Out_ PULONG64 Information,
    _Inout_ PVOID *CancelContext);
typedef NTSTATUS NTAPI WTR_EVT_SERVICE_CANCEL_REQUEST(
    _In_ WDFDEVICE ServiceDevice,
    _In_ PVOID SessionContext,
    _In_ WDFREQUEST Request,
    _Inout_ PVOID *CancelContext);
typedef VOID NTAPI WTR_EVT_SERVICE_OTHER_IO(_In_ WDFDEVICE ServiceDevice, _In_ PVOID SessionContext, _In_ WDFREQUEST Request);

struct _WTR_SERVICE_CALLBACKS
{
    ULONG Flags;
    WTR_EVT_SERVICE_ADDED *EvtServiceAdded;
    WTR_EVT_SERVICE_CLEANUP *EvtServiceCleanup;
    WTR_EVT_SERVICE_CONNECT *EvtServiceConnect;
    WTR_EVT_SERVICE_DISCONNECT *EvtServiceDisconnect;
    WTR_EVT_SERVICE_CREATE_SESSION *EvtServiceCreateSession;
    WTR_EVT_SERVICE_DESTROY_SESSION *EvtServiceDestroySession;
    WTR_EVT_SERVICE_PROCESS_REQUEST *EvtServiceProcessRequest;
    WTR_EVT_SERVICE_CANCEL_REQUEST *EvtServiceCancelRequest;
    WTR_EVT_SERVICE_OTHER_IO *EvtServiceOtherIo;
};

/* Handle the class gives each bound client */
typedef PVOID WTR_CLIENT;

typedef NTSTATUS NTAPI WTR_HANDOFF_MASTER_DEVICE(
    _In_ WTR_CLIENT Client,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PWTR_DEVICE_CALLBACKS Callbacks,
    _Out_opt_ WDFDEVICE *Device);

typedef NTSTATUS NTAPI WTR_HANDOFF_SERVICE_DEVICE(
    _In_ WTR_CLIENT Client,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ const GUID *ServiceId,
    _In_opt_ PWTR_SERVICE_CALLBACKS Callbacks,
    _Out_opt_ WDFDEVICE *Device);

typedef NTSTATUS __cdecl WTR_LOG_MESSAGE(
    _In_ WTR_CLIENT Client,
    _In_ WDFDEVICE Device,
    _In_ ULONG Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

typedef NTSTATUS NTAPI WTR_QUERY_OS_SERVICE(
    _In_ WTR_CLIENT Client,
    _In_ WDFDEVICE ServiceDevice,
    _In_ const GUID *OsServiceId,
    _Out_ PWTR_OS_SERVICE_INFORMATION Information);

typedef NTSTATUS NTAPI WTR_CALL_OS_SERVICE(
    _In_ WTR_CLIENT Client,
    _In_ WDFDEVICE ServiceDevice,
    _In_ const GUID *OsServiceId,
    _In_ PWTR_SERVICE_REQUEST Request,
    _Out_opt_ PULONG64 Information);

typedef NTSTATUS NTAPI WTR_COMPLETE_ASYNC_REQUEST(
    _In_ WTR_CLIENT Client,
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ ULONG64 Information);

typedef enum _WTR_FUNCTION_INDEX
{
    WtrHandoffMasterDeviceIndex = 0,
    WtrHandoffServiceDeviceIndex,
    WtrLogMessageIndex,
    WtrQueryOsServiceIndex,
    WtrCallOsServiceIndex,
    WtrCompleteAsyncRequestIndex,
    WtrFunctionTableNumEntries
} WTR_FUNCTION_INDEX;
