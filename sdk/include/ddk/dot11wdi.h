/*
 * PROJECT:     ReactOS WDI support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WLAN Device Driver Interface, between a WLAN miniport and the
 *              WDI upper edge above it
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Versions a WLAN miniport can declare in NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS::WdiVersion */
#define WDI_VERSION_1_0                     ((1 << 16) | (0 << 8) | 0x0)
#define WDI_VERSION_1_0_1                   ((1 << 16) | (0 << 8) | 0x1)
#define WDI_VERSION_1_0_10                  ((1 << 16) | (0 << 8) | 0xA)
#define WDI_VERSION_1_0_20                  ((1 << 16) | (0 << 8) | 0x14)
#define WDI_VERSION_1_0_21                  ((1 << 16) | (0 << 8) | 0x15)
#define WDI_VERSION_1_1_0                   ((1 << 16) | (1 << 8) | 0x0)
#define WDI_VERSION_1_1_4                   ((1 << 16) | (1 << 8) | 0x4)
#define WDI_VERSION_1_1_5                   ((1 << 16) | (1 << 8) | 0x5)
#define WDI_VERSION_1_1_6                   ((1 << 16) | (1 << 8) | 0x6)
#define WDI_VERSION_1_1_7                   ((1 << 16) | (1 << 8) | 0x7)
#define WDI_VERSION_1_1_8                   ((1 << 16) | (1 << 8) | 0x8)
#define WDI_VERSION_1_1_9                   ((1 << 16) | (1 << 8) | 0x9)
#define WDI_VERSION_1_1_10                  ((1 << 16) | (1 << 8) | 0xA)
#define WDI_VERSION_1_1_11                  ((1 << 16) | (1 << 8) | 0xB)
#define WDI_VERSION_1_1_12                  ((1 << 16) | (1 << 8) | 0xC)
#define WDI_VERSION_1_1_13                  ((1 << 16) | (1 << 8) | 0xD)
#define WDI_VERSION_LATEST                  WDI_VERSION_1_1_13

/* Data path, declared in full with the data path itself */

typedef NDIS_HANDLE TAL_TXRX_HANDLE, *PTAL_TXRX_HANDLE;

typedef struct _NDIS_WDI_DATA_API NDIS_WDI_DATA_API, *PNDIS_WDI_DATA_API;
typedef struct _NDIS_MINIPORT_WDI_DATA_HANDLERS NDIS_MINIPORT_WDI_DATA_HANDLERS, *PNDIS_MINIPORT_WDI_DATA_HANDLERS;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
(NTAPI MINIPORT_WDI_TAL_TXRX_INITIALIZE)(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ PNDIS_WDI_DATA_API NdisWdiDataPathApi,
    _Out_ PTAL_TXRX_HANDLE MiniportTalTxRxContext,
    _Inout_ PNDIS_MINIPORT_WDI_DATA_HANDLERS MiniportDataHandlers,
    _Out_ UINT32 *MiniportWdiFrameMetadataExtraSpace);
typedef MINIPORT_WDI_TAL_TXRX_INITIALIZE *MINIPORT_WDI_TAL_TXRX_INITIALIZE_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
VOID
(NTAPI MINIPORT_WDI_TAL_TXRX_DEINITIALIZE)(
    _In_ TAL_TXRX_HANDLE MiniportTalTxRxContext);
typedef MINIPORT_WDI_TAL_TXRX_DEINITIALIZE *MINIPORT_WDI_TAL_TXRX_DEINITIALIZE_HANDLER;

/* What the upper edge hands the miniport for the control path */

typedef
VOID
(NTAPI NDIS_WDI_OPEN_ADAPTER_COMPLETE)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_STATUS CompletionStatus);
typedef NDIS_WDI_OPEN_ADAPTER_COMPLETE *NDIS_WDI_OPEN_ADAPTER_COMPLETE_HANDLER;

typedef
VOID
(NTAPI NDIS_WDI_CLOSE_ADAPTER_COMPLETE)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_STATUS CompletionStatus);
typedef NDIS_WDI_CLOSE_ADAPTER_COMPLETE *NDIS_WDI_CLOSE_ADAPTER_COMPLETE_HANDLER;

/* For USB selective suspend the state confirmed has to be D2 */
typedef
VOID
(NTAPI NDIS_WDI_IDLE_NOTIFICATION_CONFIRM)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_DEVICE_POWER_STATE DeviceIdlePowerState);
typedef NDIS_WDI_IDLE_NOTIFICATION_CONFIRM *NDIS_WDI_IDLE_NOTIFICATION_CONFIRM_HANDLER;

typedef
VOID
(NTAPI NDIS_WDI_IDLE_NOTIFICATION_COMPLETE)(
    _In_ NDIS_HANDLE MiniportAdapterHandle);
typedef NDIS_WDI_IDLE_NOTIFICATION_COMPLETE *NDIS_WDI_IDLE_NOTIFICATION_COMPLETE_HANDLER;

#define NDIS_OBJECT_TYPE_WDI_INIT_PARAMETERS                0xC3
#define NDIS_OBJECT_TYPE_WDI_INIT_PARAMETERS_REVISION_1     1

typedef struct _NDIS_WDI_INIT_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    ULONG WdiVersion;
    NDIS_WDI_OPEN_ADAPTER_COMPLETE_HANDLER OpenAdapterCompleteHandler;
    NDIS_WDI_CLOSE_ADAPTER_COMPLETE_HANDLER CloseAdapterCompleteHandler;
    NDIS_WDI_IDLE_NOTIFICATION_CONFIRM_HANDLER UeIdleNotificationConfirm;
    NDIS_WDI_IDLE_NOTIFICATION_COMPLETE_HANDLER UeIdleNotificationComplete;
} NDIS_WDI_INIT_PARAMETERS, *PNDIS_WDI_INIT_PARAMETERS;

#define NDIS_SIZEOF_WDI_INIT_PARAMETERS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_WDI_INIT_PARAMETERS, UeIdleNotificationComplete)

/* Control path handlers of the miniport */

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
(NTAPI MINIPORT_WDI_ALLOCATE_ADAPTER)(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters,
    _In_ PNDIS_WDI_INIT_PARAMETERS NdisWdiInitParameters,
    _Inout_ PNDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES RegistrationAttributes);
typedef MINIPORT_WDI_ALLOCATE_ADAPTER *MINIPORT_WDI_ALLOCATE_ADAPTER_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
VOID
(NTAPI MINIPORT_WDI_FREE_ADAPTER)(
    _In_ NDIS_HANDLE MiniportAdapterContext);
typedef MINIPORT_WDI_FREE_ADAPTER *MINIPORT_WDI_FREE_ADAPTER_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
(NTAPI MINIPORT_WDI_OPEN_ADAPTER)(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters);
typedef MINIPORT_WDI_OPEN_ADAPTER *MINIPORT_WDI_OPEN_ADAPTER_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
(NTAPI MINIPORT_WDI_CLOSE_ADAPTER)(
    _In_ NDIS_HANDLE MiniportAdapterContext);
typedef MINIPORT_WDI_CLOSE_ADAPTER *MINIPORT_WDI_CLOSE_ADAPTER_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
(NTAPI MINIPORT_WDI_START_ADAPTER_OPERATION)(
    _In_ NDIS_HANDLE MiniportAdapterContext);
typedef MINIPORT_WDI_START_ADAPTER_OPERATION *MINIPORT_WDI_START_OPERATION_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
VOID
(NTAPI MINIPORT_WDI_STOP_ADAPTER_OPERATION)(
    _In_ NDIS_HANDLE MiniportAdapterContext);
typedef MINIPORT_WDI_STOP_ADAPTER_OPERATION *MINIPORT_WDI_STOP_OPERATION_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
(NTAPI MINIPORT_WDI_POST_ADAPTER_PAUSE)(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters);
typedef MINIPORT_WDI_POST_ADAPTER_PAUSE *MINIPORT_WDI_POST_PAUSE_HANDLER;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
(NTAPI MINIPORT_WDI_POST_ADAPTER_RESTART)(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters);
typedef MINIPORT_WDI_POST_ADAPTER_RESTART *MINIPORT_WDI_POST_RESTART_HANDLER;

typedef enum
{
    DiagnoseLevelNone = 0,
    DiagnoseLevelHardwareRegisters = 1,
    DiagnoseLevelFirmwareImageDump = 2,
    DiagnoseLevelDriverStateDump = 3
} eDiagnoseLevel;

typedef
NDIS_STATUS
(NTAPI MINIPORT_WDI_ADAPTER_HANG_DIAGNOSE)(
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ eDiagnoseLevel DiagnoseLevel,
    _In_ UINT32 BufferSize,
    _Out_writes_bytes_to_(BufferSize, *OutputSize) UINT8 *FirmwareBlob,
    _Out_ UINT32 *OutputSize);
typedef MINIPORT_WDI_ADAPTER_HANG_DIAGNOSE *MINIPORT_WDI_HANG_DIAGNOSE_HANDLER;

/* USB selective suspend */
typedef
NDIS_STATUS
(NTAPI MINIPORT_WDI_IDLE_NOTIFICATION)(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ BOOLEAN ForceIdle);
typedef MINIPORT_WDI_IDLE_NOTIFICATION *MINIPORT_WDI_IDLE_NOTIFICATION_HANDLER;

typedef
VOID
(NTAPI MINIPORT_WDI_CANCEL_IDLE_NOTIFICATION)(
    _In_ NDIS_HANDLE MiniportAdapterContext);
typedef MINIPORT_WDI_CANCEL_IDLE_NOTIFICATION *MINIPORT_WDI_CANCEL_IDLE_NOTIFICATION_HANDLER;

#define NDIS_OBJECT_TYPE_MINIPORT_WDI_CHARACTERISTICS               0xC0
#define NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS_REVISION_1         1

typedef struct _NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS
{
    NDIS_OBJECT_HEADER Header;
    ULONG WdiVersion;
    MINIPORT_WDI_ALLOCATE_ADAPTER_HANDLER AllocateAdapterHandler;
    MINIPORT_WDI_FREE_ADAPTER_HANDLER FreeAdapterHandler;
    MINIPORT_WDI_OPEN_ADAPTER_HANDLER OpenAdapterHandler;
    MINIPORT_WDI_CLOSE_ADAPTER_HANDLER CloseAdapterHandler;
    MINIPORT_WDI_START_OPERATION_HANDLER StartOperationHandler;
    MINIPORT_WDI_STOP_OPERATION_HANDLER StopOperationHandler;
    MINIPORT_WDI_POST_PAUSE_HANDLER PostPauseHandler;
    MINIPORT_WDI_POST_RESTART_HANDLER PostRestartHandler;
    MINIPORT_WDI_HANG_DIAGNOSE_HANDLER HangDiagnoseHandler;
    MINIPORT_WDI_TAL_TXRX_INITIALIZE_HANDLER TalTxRxInitializeHandler;
    MINIPORT_WDI_TAL_TXRX_DEINITIALIZE_HANDLER TalTxRxDeinitializeHandler;
    MINIPORT_WDI_IDLE_NOTIFICATION_HANDLER LeIdleNotificationHandler;
    MINIPORT_WDI_CANCEL_IDLE_NOTIFICATION_HANDLER LeCancelIdleNotificationHandler;
} NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS, *PNDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS;

#define NDIS_SIZEOF_MINIPORT_WDI_CHARACTERISTICS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS, LeCancelIdleNotificationHandler)

_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisMRegisterWdiMiniportDriver(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _In_opt_ NDIS_HANDLE NdisDriverContext,
    _In_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    _In_ PNDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS MiniportWdiCharacteristics,
    _Out_ PNDIS_HANDLE NdisMiniportDriverHandle);

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
NdisMDeregisterWdiMiniportDriver(
    _In_ NDIS_HANDLE NdisMiniportDriverHandle);

#ifdef __cplusplus
}
#endif
