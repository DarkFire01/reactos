/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The interface a WDF class extension binds to NDIS with
 *
 * Only a class extension uses this. A miniport never sees it.
 */

#pragma once

#include <ndis.h>
#include <ntddndis_p.h>

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NDIS_WDF_CX_DRIVER);
DECLARE_HANDLE(NDIS_WDF_CX_DRIVER_CONTEXT);
DECLARE_HANDLE(NDIS_WDF_CX_HANDLE);

#define NDIS_WDF_CX_CHARACTERISTICS_REVISION_1      1

typedef enum _NdisWdfPnpPowerAction
{
    NdisWdfActionPnpStart = 0,
    NdisWdfActionPnpQueryStop,
    NdisWdfActionPnpCancelStop,
    NdisWdfActionPnpStop,
    NdisWdfActionPnpQueryRemove,
    NdisWdfActionPnpCancelRemove,
    NdisWdfActionPnpSurpriseRemove,
    NdisWdfActionPnpRemove,
    NdisWdfActionPowerD0Implicit,
    NdisWdfActionPowerD0,
    NdisWdfActionPowerDx,
    NdisWdfActionStartPowerManagement,
    NdisWdfActionStopPowerManagement,
    NdisWdfActionPowerDxFinal,
    NdisWdfActionPowerDxOnSystemSx,
    NdisWdfActionPowerDxOnSystemShutdown,
    NdisWdfActionPowerNone,
    NdisWdfActionPreReleaseHardware,
    NdisWdfActionPostReleaseHardware,
    NdisWdfActionPnpRebalance,
    NdisWdfActionDeviceObjectCleanup
} NDIS_WDF_PNP_POWER_ACTION, *PNDIS_WDF_PNP_POWER_ACTION;

typedef enum _NET_DEVICE_RESET_TYPE
{
    FunctionLevelReset = 0,
    PlatformLevelReset
} NET_DEVICE_RESET_TYPE, *PNET_DEVICE_RESET_TYPE;

typedef enum _NDIS_CX_STUCK_OPERATION_TYPE
{
    NdisCxStuckOperationNetBufferList = 0,
    NdisCxStuckOperationOidRequest
} NDIS_CX_STUCK_OPERATION_TYPE, *PNDIS_CX_STUCK_OPERATION_TYPE;

typedef struct _NDIS_WDF_ADD_DEVICE_INFO
{
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PVOID MiniportAdapterContext;
} NDIS_WDF_ADD_DEVICE_INFO, *PNDIS_WDF_ADD_DEVICE_INFO;

typedef struct _NDIS_WDF_COMPLETE_ADD_PARAMS
{
    GUID InterfaceGuid;
    NET_LUID NetLuid;
    NDIS_MEDIUM MediaType;
    UNICODE_STRING BaseName;
    UNICODE_STRING AdapterInstanceName;
    UNICODE_STRING DriverImageName;
    PVOID ExecutionContextKnobs;
} NDIS_WDF_COMPLETE_ADD_PARAMS, *PNDIS_WDF_COMPLETE_ADD_PARAMS;

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
(NTAPI EVT_NDIS_WDF_MINIPORT_POWER_REFERENCE)(
    _In_ NDIS_HANDLE Adapter,
    _In_ BOOLEAN WaitForD0,
    _In_ BOOLEAN InvokeCompletionCallback);

typedef EVT_NDIS_WDF_MINIPORT_POWER_REFERENCE *PFN_NDIS_WDF_MINIPORT_POWER_REFERENCE;

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
(NTAPI EVT_NDIS_WDF_MINIPORT_POWER_DEREFERENCE)(
    _In_ NDIS_HANDLE Adapter);

typedef EVT_NDIS_WDF_MINIPORT_POWER_DEREFERENCE *PFN_NDIS_WDF_MINIPORT_POWER_DEREFERENCE;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI EVT_NDIS_WDF_MINIPORT_UPDATE_IDLE_CONDITION)(
    _In_ NDIS_HANDLE Adapter,
    _In_ NDIS_IDLE_CONDITION IdleCondition);

typedef EVT_NDIS_WDF_MINIPORT_UPDATE_IDLE_CONDITION *PFN_NDIS_WDF_MINIPORT_UPDATE_IDLE_CONDITION;

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
PDEVICE_OBJECT
(NTAPI EVT_NDIS_WDF_MINIPORT_GET_DEVICE_OBJECT)(
    _In_ NDIS_HANDLE Adapter);

typedef EVT_NDIS_WDF_MINIPORT_GET_DEVICE_OBJECT *PFN_NDIS_WDF_MINIPORT_GET_DEVICE_OBJECT;

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
PDEVICE_OBJECT
(NTAPI EVT_NDIS_WDF_MINIPORT_GET_NEXT_DEVICE_OBJECT)(
    _In_ NDIS_HANDLE Adapter);

typedef EVT_NDIS_WDF_MINIPORT_GET_NEXT_DEVICE_OBJECT *PFN_NDIS_WDF_MINIPORT_GET_NEXT_DEVICE_OBJECT;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI EVT_NDIS_WDF_MINIPORT_GET_ASSIGNED_FDO_NAME)(
    _In_ NDIS_HANDLE Adapter,
    _Out_ PUNICODE_STRING FdoName);

typedef EVT_NDIS_WDF_MINIPORT_GET_ASSIGNED_FDO_NAME *PFN_NDIS_WDF_MINIPORT_GET_ASSIGNED_FDO_NAME;

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_HANDLE
(NTAPI EVT_NDIS_WDF_MINIPORT_GET_NDIS_HANDLE_FROM_DEVICE_OBJECT)(
    _In_ PDEVICE_OBJECT DeviceObject);

typedef EVT_NDIS_WDF_MINIPORT_GET_NDIS_HANDLE_FROM_DEVICE_OBJECT
    *PFN_NDIS_WDF_MINIPORT_GET_NDIS_HANDLE_FROM_DEVICE_OBJECT;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI EVT_NDIS_WDF_MINIPORT_UPDATE_PM_PARAMETERS)(
    _In_ NDIS_HANDLE Adapter,
    _In_ PNDIS_PM_PARAMETERS PmParameters);

typedef EVT_NDIS_WDF_MINIPORT_UPDATE_PM_PARAMETERS *PFN_NDIS_WDF_MINIPORT_UPDATE_PM_PARAMETERS;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI EVT_NDIS_WDF_MINIPORT_ALLOCATE_BLOCK)(
    _In_ NDIS_HANDLE Adapter,
    _In_ ULONG Size,
    _Outptr_ PVOID *MiniportBlock);

typedef EVT_NDIS_WDF_MINIPORT_ALLOCATE_BLOCK *PFN_NDIS_WDF_MINIPORT_ALLOCATE_BLOCK;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI EVT_NDIS_WDF_MINIPORT_COMPLETE_ADD)(
    _In_ NDIS_HANDLE Adapter,
    _In_ PNDIS_WDF_COMPLETE_ADD_PARAMS Params);

typedef EVT_NDIS_WDF_MINIPORT_COMPLETE_ADD *PFN_NDIS_WDF_MINIPORT_COMPLETE_ADD;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI EVT_NDIS_WDF_DEVICE_START_COMPLETE)(
    _In_ NDIS_HANDLE Adapter);

typedef EVT_NDIS_WDF_DEVICE_START_COMPLETE *PFN_NDIS_WDF_DEVICE_START_COMPLETE;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI EVT_NDIS_WDF_MINIPORT_DEVICE_RESET)(
    _In_ NDIS_HANDLE Adapter,
    _In_ NET_DEVICE_RESET_TYPE NetDeviceResetType);

typedef EVT_NDIS_WDF_MINIPORT_DEVICE_RESET *PFN_NDIS_WDF_MINIPORT_DEVICE_RESET;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI EVT_NDIS_WDF_MINIPORT_QUERY_DEVICE_RESET_SUPPORT)(
    _In_ NDIS_HANDLE Adapter,
    _Out_ PULONG SupportedNetDeviceResetTypes);

typedef EVT_NDIS_WDF_MINIPORT_QUERY_DEVICE_RESET_SUPPORT
    *PFN_NDIS_WDF_MINIPORT_QUERY_DEVICE_RESET_SUPPORT;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI EVT_NDIS_WDF_GET_WMI_EVENT_GUID)(
    _In_ NDIS_HANDLE Adapter,
    _In_ NTSTATUS GuidStatus,
    _Outptr_ PNDIS_GUID *Guid);

typedef EVT_NDIS_WDF_GET_WMI_EVENT_GUID *PFN_NDIS_WDF_GET_WMI_EVENT_GUID;

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
(NTAPI EVT_NDIS_WDF_REPORT_STUCK_OPERATION)(
    _In_ NDIS_HANDLE Adapter,
    _In_ NDIS_CX_STUCK_OPERATION_TYPE OperationType,
    _In_ ULONG64 Operation);

typedef EVT_NDIS_WDF_REPORT_STUCK_OPERATION *PFN_NDIS_WDF_REPORT_STUCK_OPERATION;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI EVT_NDIS_WDF_MINIPORT_AOAC_ENGAGE)(
    _In_ NDIS_HANDLE Adapter);

typedef EVT_NDIS_WDF_MINIPORT_AOAC_ENGAGE *PFN_NDIS_WDF_MINIPORT_AOAC_ENGAGE;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI EVT_NDIS_WDF_MINIPORT_AOAC_DISENGAGE)(
    _In_ NDIS_HANDLE Adapter,
    _In_ BOOLEAN WaitForD0);

typedef EVT_NDIS_WDF_MINIPORT_AOAC_DISENGAGE *PFN_NDIS_WDF_MINIPORT_AOAC_DISENGAGE;

typedef struct _NDIS_WDF_CX_CHARACTERISTICS
{
    NDIS_OBJECT_HEADER Header;
    PFN_NDIS_WDF_MINIPORT_POWER_REFERENCE EvtCxPowerReference;
    PFN_NDIS_WDF_MINIPORT_POWER_DEREFERENCE EvtCxPowerDereference;
    PFN_NDIS_WDF_MINIPORT_UPDATE_IDLE_CONDITION EvtCxUpdateIdleCondition;
    PFN_NDIS_WDF_MINIPORT_GET_DEVICE_OBJECT EvtCxGetDeviceObject;
    PFN_NDIS_WDF_MINIPORT_GET_NEXT_DEVICE_OBJECT EvtCxGetNextDeviceObject;
    PFN_NDIS_WDF_MINIPORT_GET_ASSIGNED_FDO_NAME EvtCxGetAssignedFdoName;
    PFN_NDIS_WDF_MINIPORT_GET_NDIS_HANDLE_FROM_DEVICE_OBJECT EvtCxGetNdisHandleFromDeviceObject;
    PFN_NDIS_WDF_MINIPORT_UPDATE_PM_PARAMETERS EvtCxUpdatePMParameters;
    PFN_NDIS_WDF_MINIPORT_ALLOCATE_BLOCK EvtCxAllocateMiniportBlock;
    PFN_NDIS_WDF_MINIPORT_COMPLETE_ADD EvtCxMiniportCompleteAdd;
    PFN_NDIS_WDF_DEVICE_START_COMPLETE EvtCxDeviceStartComplete;
    PFN_NDIS_WDF_MINIPORT_DEVICE_RESET EvtCxMiniportDeviceReset;
    PFN_NDIS_WDF_MINIPORT_QUERY_DEVICE_RESET_SUPPORT EvtCxMiniportQueryDeviceResetSupport;
    PFN_NDIS_WDF_GET_WMI_EVENT_GUID EvtCxGetWmiEventGuid;
    PFN_NDIS_WDF_REPORT_STUCK_OPERATION EvtCxReportStuckOperation;
} NDIS_WDF_CX_CHARACTERISTICS, *PNDIS_WDF_CX_CHARACTERISTICS;

#define NDIS_WDF_CX_CHARACTERISTICS_INIT(Characteristics)                       \
{                                                                               \
    RtlZeroMemory((Characteristics), sizeof(NDIS_WDF_CX_CHARACTERISTICS));      \
    (Characteristics)->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;                  \
    (Characteristics)->Header.Revision = NDIS_WDF_CX_CHARACTERISTICS_REVISION_1;\
    (Characteristics)->Header.Size = sizeof(NDIS_WDF_CX_CHARACTERISTICS);       \
}

NDISAPI
NTSTATUS
NTAPI
NdisWdfRegisterCx(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_opt_ NDIS_WDF_CX_DRIVER_CONTEXT CxDriverContext,
    _In_ PNDIS_WDF_CX_CHARACTERISTICS CxDriverCharacteristics,
    _Out_ NDIS_WDF_CX_DRIVER *NdisCxDriverHandle);

NDISAPI
VOID
NTAPI
NdisWdfDeregisterCx(
    _In_ NDIS_WDF_CX_DRIVER NdisCxDriverHandle);

NDISAPI
NTSTATUS
NTAPI
NdisWdfRegisterMiniportDriver(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_ NDIS_WDF_CX_DRIVER NdisCxDriverHandle,
    _In_opt_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    _Out_ PNDIS_HANDLE NdisMiniportDriverHandle);

NDISAPI
NTSTATUS
NTAPI
NdisWdfPnpAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PNDIS_HANDLE NdisAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterContext);

NDISAPI
NTSTATUS
NTAPI
NdisWdfPnpPowerEventHandler(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_WDF_PNP_POWER_ACTION PnpPowerAction);

NDISAPI
VOID
NTAPI
NdisWdfMiniportSetPower(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ POWER_ACTION SystemPowerAction,
    _In_ DEVICE_POWER_STATE DevicePowerState);

NDISAPI
BOOLEAN
NTAPI
NdisWdfMiniportTryReference(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

NDISAPI
VOID
NTAPI
NdisWdfMiniportDereference(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

NDISAPI
VOID
NTAPI
NdisWdfMiniportStarted(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

NDISAPI
VOID
NTAPI
NdisWdfMiniportDataPathStart(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

NDISAPI
VOID
NTAPI
NdisWdfMiniportDataPathPause(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

NDISAPI
PVOID
NTAPI
NdisWdfGetAdapterContextFromAdapterHandle(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

NDISAPI
VOID
NTAPI
NdisWdfAsyncPowerReferenceCompleteNotification(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NTSTATUS StatusOfOperation);

NDISAPI
VOID
NTAPI
NdisWdfNotifyWmiAdapterArrival(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

NDISAPI
VOID
NTAPI
NdisWdfReadConfiguration(
    _Out_ PNDIS_STATUS Status,
    _Outptr_ PNDIS_CONFIGURATION_PARAMETER *ParameterValue,
    _In_ NDIS_HANDLE ConfigurationHandle,
    _In_ PUNICODE_STRING Keyword,
    _In_ NDIS_PARAMETER_TYPE ParameterType);

NDISAPI
NTSTATUS
NTAPI
NdisWdfCreateIrpHandler(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PIRP Irp);

NDISAPI
NTSTATUS
NTAPI
NdisWdfCloseIrpHandler(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PIRP Irp);

NDISAPI
NTSTATUS
NTAPI
NdisWdfDeviceControlIrpHandler(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PIRP Irp);

NDISAPI
NTSTATUS
NTAPI
NdisWdfDeviceInternalControlIrpHandler(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PIRP Irp);

NDISAPI
VOID
NTAPI
NdisWdfGetGuidToOidMap(
    _In_reads_(OidCount) PNDIS_OID OidList,
    _In_ USHORT OidCount,
    _Out_writes_opt_(*GuidToOidCount) PNDIS_GUID GuidToOidMap,
    _Inout_ PULONG GuidToOidCount);

NDISAPI
NTSTATUS
NTAPI
NdisWdfQueryAllData(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_GUID NdisGuid,
    _In_ LPCGUID Guid,
    _Out_writes_bytes_(BufferSize) PVOID DataBuffer,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize);

NDISAPI
NTSTATUS
NTAPI
NdisWdfQuerySingleInstance(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ LPCGUID Guid,
    _Inout_ PVOID Wnode,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize);

NDISAPI
NTSTATUS
NTAPI
NdisWdfChangeSingleInstance(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ LPCGUID Guid,
    _In_ PVOID Wnode);

NDISAPI
NTSTATUS
NTAPI
NdisWdfExecuteMethod(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ LPCGUID Guid,
    _Inout_ PVOID Wnode,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnSize);

#ifdef __cplusplus
}
#endif
