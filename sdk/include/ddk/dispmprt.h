/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Dispmprt public header
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#include <d3dkmddi.h>
typedef enum _DXGK_EVENT_TYPE {
    DxgkUndefinedEvent,
    DxgkAcpiEvent,
    DxgkPowerStateEvent,
    DxgkDockingEvent,
    DxgkChainedAcpiEvent
} DXGK_EVENT_TYPE, *PDXGK_EVENT_TYPE;

typedef struct _DXGK_VIDEO_OUTPUT_CAPABILITIES {
    D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY InterfaceTechnology;
    D3DKMDT_MONITOR_ORIENTATION_AWARENESS MonitorOrientationAwareness;
    BOOLEAN SupportsSdtvModes;
} DXGK_VIDEO_OUTPUT_CAPABILITIES, *PDXGK_VIDEO_OUTPUT_CAPABILITIES;

typedef struct _DXGK_INTEGRATED_DISPLAY_CHILD {
    D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY InterfaceTechnology;
    USHORT                          DescriptorLength;
} DXGK_INTEGRATED_DISPLAY_CHILD, *PDXGK_INTEGRATED_DISPLAY_CHILD;

typedef struct _DXGK_CHILD_CAPABILITIES {

    union
    {

        DXGK_VIDEO_OUTPUT_CAPABILITIES  VideoOutput;
        struct
        {
            UINT MustBeZero;
        }
        Other;
        DXGK_INTEGRATED_DISPLAY_CHILD   IntegratedDisplayChild;
    } Type;

    DXGK_CHILD_DEVICE_HPD_AWARENESS HpdAwareness;
} DXGK_CHILD_CAPABILITIES, *PDXGK_CHILD_CAPABILITIES;

typedef enum _DXGK_CHILD_DEVICE_TYPE {
   TypeUninitialized,
   TypeVideoOutput,
   TypeOther,
   TypeIntegratedDisplay
} DXGK_CHILD_DEVICE_TYPE, *PDXGK_CHILD_DEVICE_TYPE;

typedef struct _DXGK_CHILD_DESCRIPTOR {
   DXGK_CHILD_DEVICE_TYPE ChildDeviceType;
   DXGK_CHILD_CAPABILITIES ChildCapabilities;
   ULONG AcpiUid;
   ULONG ChildUid;
} DXGK_CHILD_DESCRIPTOR, *PDXGK_CHILD_DESCRIPTOR;

typedef struct _DXGK_DEVICE_DESCRIPTOR {
   ULONG DescriptorOffset;
   ULONG DescriptorLength;
   _Field_size_bytes_DXGK_(DescriptorLength) PVOID DescriptorBuffer;
} DXGK_DEVICE_DESCRIPTOR, *PDXGK_DEVICE_DESCRIPTOR;

typedef struct _DXGKRNL_INTERFACE {
    ULONG                                   Size;
    ULONG                                   Version;
    HANDLE                                  DeviceHandle;
    //TODO:
} DXGKRNL_INTERFACE, *PDXGKRNL_INTERFACE;

typedef struct _DXGK_START_INFO {
    ULONG                          RequiredDmaQueueEntry;
    GUID                           AdapterGuid;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    LUID                           AdapterLuid;
#endif

} DXGK_START_INFO, *PDXGK_START_INFO;

typedef
NTSTATUS
NTAPI
DXGKDDI_ADD_DEVICE(_In_ PDEVICE_OBJECT PhysicalDeviceObject,
                   _Outptr_ PVOID*     MiniportDeviceContext);

typedef
NTSTATUS
NTAPI
DXGKDDI_START_DEVICE(_In_  PVOID               MiniportDeviceContext,
                     _In_  PDXGK_START_INFO    DxgkStartInfo,
                     _In_  PDXGKRNL_INTERFACE  DxgkInterface,
                     _Out_ PULONG              NumberOfVideoPresentSources,
                     _Out_ PULONG              NumberOfChildren);

typedef
NTSTATUS
DXGKDDI_STOP_DEVICE(_In_ PVOID  MiniportDeviceContext);

typedef
NTSTATUS
DXGKDDI_REMOVE_DEVICE(_In_ PVOID  MiniportDeviceContext);

typedef
NTSTATUS
DXGKDDI_DISPATCH_IO_REQUEST(_In_ PVOID              MiniportDeviceContext,
                            _In_ ULONG                    VidPnSourceId,
                            _In_ PVIDEO_REQUEST_PACKET    VideoRequestPacket);

typedef
NTSTATUS
DXGKDDI_QUERY_CHILD_RELATIONS(
    _In_ PVOID                                                    MiniportDeviceContext,
    _Inout_updates_bytes_(ChildRelationsSize) PDXGK_CHILD_DESCRIPTOR  ChildRelations,
    _In_ ULONG                                                        ChildRelationsSize
    );

typedef
NTSTATUS
DXGKDDI_QUERY_CHILD_STATUS(_In_ PVOID                  MiniportDeviceContext,
                           _Inout_ PDXGK_CHILD_STATUS  ChildStatus,
                           _In_ BOOLEAN                NonDestructiveOnly
    );

typedef
BOOLEAN
DXGKDDI_INTERRUPT_ROUTINE(_In_ PVOID  MiniportDeviceContext,
                          _In_ ULONG        MessageNumber);

typedef
VOID
DXGKDDI_DPC_ROUTINE(_In_ PVOID  MiniportDeviceContext);

typedef
NTSTATUS
DXGKDDI_QUERY_DEVICE_DESCRIPTOR(
    _In_ PVOID                  MiniportDeviceContext,
    _In_ ULONG                        ChildUid,
    _Inout_ PDXGK_DEVICE_DESCRIPTOR   DeviceDescriptor
    );

typedef
    _Check_return_
_Function_class_DXGK_(DXGKDDI_SET_POWER_STATE)
_IRQL_requires_DXGK_(PASSIVE_LEVEL)
NTSTATUS
DXGKDDI_SET_POWER_STATE(
    _In_ PVOID          MiniportDeviceContext,
    _In_ ULONG                DeviceUid,
    _In_ DEVICE_POWER_STATE   DevicePowerState,
    _In_ POWER_ACTION         ActionType
    );

typedef
NTSTATUS
DXGKDDI_NOTIFY_ACPI_EVENT(
    _In_ PVOID      MiniportDeviceContext,
    _In_ DXGK_EVENT_TYPE  EventType,
    _In_ ULONG            Event,
    _In_ PVOID            Argument,
    _Out_ PULONG          AcpiFlags
    );

typedef
VOID
DXGKDDI_RESET_DEVICE(
    IN_CONST_PVOID  MiniportDeviceContext
    );

typedef
VOID
DXGKDDI_UNLOAD(
    VOID
    );

typedef
NTSTATUS
DXGKDDI_QUERY_INTERFACE(
    _In_ PVOID          MiniportDeviceContext,
    _In_ PQUERY_INTERFACE     QueryInterface
    );

typedef
VOID
DXGKDDI_CONTROL_ETW_LOGGING(
    _IN_ BOOLEAN  Enable,
    _IN_ ULONG    Flags,
    _IN_ UCHAR    Level
    );

typedef
NTSTATUS
DXGKDDI_LINK_DEVICE(
    _In_ PDEVICE_OBJECT   PhysicalDeviceObject,
    _In_ PVOID            MiniportDeviceContext,
    _Inout_ PLINKED_DEVICE      LinkedDevice
    );

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_2)

typedef struct _DXGK_PRE_START_INFO {
    union {
        struct {
            UINT    ReservedIn;
        };
        UINT Input;
    };
    union {
        struct {
            UINT    SupportPreserveBootDisplay  : 1;
            UINT    IsUEFIFrameBufferCpuAccessibleDuringStartup : 1;
            UINT    ReservedOut                 :30;
        };
        UINT Output;
    };
} DXGK_PRE_START_INFO, *PDXGK_PRE_START_INFO;

typedef
NTSTATUS
DXGKDDI_EXCHANGEPRESTARTINFO(_In_ HANDLE                  hAdapter,
                             _Inout_ PDXGK_PRE_START_INFO pPreStartInfo);

typedef
NTSTATUS
APIENTRY
DXGKDDI_SETTARGETADJUSTEDCOLORIMETRY(
    _In_ HANDLE                                 hAdapter,
    _In_  D3DDDI_VIDEO_PRESENT_TARGET_ID              TargetId,
    _In_  DXGK_COLORIMETRY                            AdjustedColorimetry
    );

#endif

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_4)

typedef struct _DXGKARG_QUERYDIAGNOSTICTYPESSUPPORT
{
    _In_  DXGK_DIAGNOSTIC_CATEGORIES    DiagnosticCategory;
    _Out_ DXGK_DIAGNOSTIC_TYPES         NoninvasiveTypes;
    _Out_ DXGK_DIAGNOSTIC_TYPES         InvasiveTypes;
} DXGKARG_QUERYDIAGNOSTICTYPESSUPPORT, *PDXGKARG_QUERYDIAGNOSTICTYPESSUPPORT;

typedef
NTSTATUS
APIENTRY
DXGKDDI_QUERYDIAGNOSTICTYPESSUPPORT(_In_ PVOID                              MiniportDeviceContext,
                                    _Inout_ PDXGKARG_QUERYDIAGNOSTICTYPESSUPPORT  pArgQueryDiagnosticTypesSupport);

typedef struct _DXGKARG_CONTROLDIAGNOSTICREPORTING
{
    _In_  DXGK_DIAGNOSTIC_CATEGORIES    DiagnosticCategory;
    _In_  DXGK_DIAGNOSTIC_TYPES         RequestedDiagnostics;
} DXGKARG_CONTROLDIAGNOSTICREPORTING, *PDXGKARG_CONTROLDIAGNOSTICREPORTING;

typedef
NTSTATUS
APIENTRY
DXGKDDI_CONTROLDIAGNOSTICREPORTING(
    _In_ PVOID                          MiniportDeviceContext,
    _In_ PDXGKARG_CONTROLDIAGNOSTICREPORTING  pArgControlDiagnosticReporting
    );

#endif

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_5)

typedef struct _DXGKARG_SETTARGETADJUSTEDCOLORIMETRY2
{
    _In_  D3DDDI_VIDEO_PRESENT_TARGET_ID              TargetId;
    _In_  DXGK_COLORIMETRY                            AdjustedColorimetry;
    _In_  UINT                                        SdrWhiteLevel;            // SDR white level in integer nits
} DXGKARG_SETTARGETADJUSTEDCOLORIMETRY2, *PDXGKARG_SETTARGETADJUSTEDCOLORIMETRY2;

typedef
NTSTATUS
APIENTRY
DXGKDDI_SETTARGETADJUSTEDCOLORIMETRY2(
    _In_ CONST_HANDLE                                 hAdapter,
    _In_ PDXGKARG_SETTARGETADJUSTEDCOLORIMETRY2       pArgSetTargetAdjustedColorimetry
    );
#endif
typedef DXGKDDI_ADD_DEVICE                      *PDXGKDDI_ADD_DEVICE;
typedef DXGKDDI_START_DEVICE                    *PDXGKDDI_START_DEVICE;
typedef DXGKDDI_STOP_DEVICE                     *PDXGKDDI_STOP_DEVICE;
typedef DXGKDDI_REMOVE_DEVICE                   *PDXGKDDI_REMOVE_DEVICE;
typedef DXGKDDI_DISPATCH_IO_REQUEST             *PDXGKDDI_DISPATCH_IO_REQUEST;
typedef DXGKDDI_QUERY_CHILD_RELATIONS           *PDXGKDDI_QUERY_CHILD_RELATIONS;
typedef DXGKDDI_QUERY_CHILD_STATUS              *PDXGKDDI_QUERY_CHILD_STATUS;
typedef DXGKDDI_INTERRUPT_ROUTINE               *PDXGKDDI_INTERRUPT_ROUTINE;
typedef DXGKDDI_DPC_ROUTINE                     *PDXGKDDI_DPC_ROUTINE;
typedef DXGKDDI_QUERY_DEVICE_DESCRIPTOR         *PDXGKDDI_QUERY_DEVICE_DESCRIPTOR;
typedef DXGKDDI_SET_POWER_STATE                 *PDXGKDDI_SET_POWER_STATE;
typedef DXGKDDI_NOTIFY_ACPI_EVENT               *PDXGKDDI_NOTIFY_ACPI_EVENT;
typedef DXGKDDI_RESET_DEVICE                    *PDXGKDDI_RESET_DEVICE;
typedef DXGKDDI_UNLOAD                          *PDXGKDDI_UNLOAD;
typedef DXGKDDI_QUERY_INTERFACE                 *PDXGKDDI_QUERY_INTERFACE;
typedef DXGKDDI_CONTROL_ETW_LOGGING             *PDXGKDDI_CONTROL_ETW_LOGGING;
typedef DXGKDDI_LINK_DEVICE                     *PDXGKDDI_LINK_DEVICE;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_2)
typedef DXGKDDI_EXCHANGEPRESTARTINFO            *PDXGKDDI_EXCHANGEPRESTARTINFO;
typedef DXGKDDI_SETTARGETADJUSTEDCOLORIMETRY    *PDXGKDDI_SETTARGETADJUSTEDCOLORIMETRY;
#endif
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_4)
typedef DXGKDDI_QUERYDIAGNOSTICTYPESSUPPORT     *PDXGKDDI_QUERYDIAGNOSTICTYPESSUPPORT;
typedef DXGKDDI_CONTROLDIAGNOSTICREPORTING      *PDXGKDDI_CONTROLDIAGNOSTICREPORTING;
#endif
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_5)
typedef DXGKDDI_SETTARGETADJUSTEDCOLORIMETRY2   *PDXGKDDI_SETTARGETADJUSTEDCOLORIMETRY2;
#endif


typedef struct _KMDDOD_INITIALIZATION_DATA {
    ULONG                                   Version;
    PDXGKDDI_ADD_DEVICE                     DxgkDdiAddDevice;
    PDXGKDDI_START_DEVICE                   DxgkDdiStartDevice;
    PDXGKDDI_STOP_DEVICE                    DxgkDdiStopDevice;
    PDXGKDDI_REMOVE_DEVICE                  DxgkDdiRemoveDevice;
    PDXGKDDI_DISPATCH_IO_REQUEST            DxgkDdiDispatchIoRequest;
    PDXGKDDI_INTERRUPT_ROUTINE              DxgkDdiInterruptRoutine;
    PDXGKDDI_DPC_ROUTINE                    DxgkDdiDpcRoutine;
    PDXGKDDI_QUERY_CHILD_RELATIONS          DxgkDdiQueryChildRelations;
    PDXGKDDI_QUERY_CHILD_STATUS             DxgkDdiQueryChildStatus;
    PDXGKDDI_QUERY_DEVICE_DESCRIPTOR        DxgkDdiQueryDeviceDescriptor;
    PDXGKDDI_SET_POWER_STATE                DxgkDdiSetPowerState;
    PDXGKDDI_NOTIFY_ACPI_EVENT              DxgkDdiNotifyAcpiEvent;
    PDXGKDDI_RESET_DEVICE                   DxgkDdiResetDevice;
    PDXGKDDI_UNLOAD                         DxgkDdiUnload;
    PDXGKDDI_QUERY_INTERFACE                DxgkDdiQueryInterface;
    PDXGKDDI_CONTROL_ETW_LOGGING            DxgkDdiControlEtwLogging;
    PDXGKDDI_QUERYADAPTERINFO               DxgkDdiQueryAdapterInfo;
    PDXGKDDI_SETPALETTE                     DxgkDdiSetPalette;
    PDXGKDDI_SETPOINTERPOSITION             DxgkDdiSetPointerPosition;
    PDXGKDDI_SETPOINTERSHAPE                DxgkDdiSetPointerShape;
    PDXGKDDI_ESCAPE                         DxgkDdiEscape;
    PDXGKDDI_COLLECTDBGINFO                 DxgkDdiCollectDbgInfo;
    PDXGKDDI_ISSUPPORTEDVIDPN               DxgkDdiIsSupportedVidPn;
    PDXGKDDI_RECOMMENDFUNCTIONALVIDPN       DxgkDdiRecommendFunctionalVidPn;
    PDXGKDDI_ENUMVIDPNCOFUNCMODALITY        DxgkDdiEnumVidPnCofuncModality;
    PDXGKDDI_SETVIDPNSOURCEVISIBILITY       DxgkDdiSetVidPnSourceVisibility;
    PDXGKDDI_COMMITVIDPN                    DxgkDdiCommitVidPn;
    PDXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH   DxgkDdiUpdateActiveVidPnPresentPath;
    PDXGKDDI_RECOMMENDMONITORMODES          DxgkDdiRecommendMonitorModes;
    PDXGKDDI_GETSCANLINE                    DxgkDdiGetScanLine;
    PDXGKDDI_QUERYVIDPNHWCAPABILITY         DxgkDdiQueryVidPnHWCapability;
#if 0
    //
    // New DDI for the PresentDisplayOnly function.
    //
    PDXGKDDI_PRESENTDISPLAYONLY             DxgkDdiPresentDisplayOnly;

    //
    // New DDIs for PnP stop/start support.
    //
    PDXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP DxgkDdiStopDeviceAndReleasePostDisplayOwnership;

    //
    // New DDIs for system display support.
    //
    PDXGKDDI_SYSTEM_DISPLAY_ENABLE          DxgkDdiSystemDisplayEnable;
    PDXGKDDI_SYSTEM_DISPLAY_WRITE           DxgkDdiSystemDisplayWrite;

    //
    // New DDI for the monitor container ID support.
    //
    PDXGKDDI_GET_CHILD_CONTAINER_ID         DxgkDdiGetChildContainerId;

    //
    // New DDI for HW VSync.
    //
    PDXGKDDI_CONTROLINTERRUPT               DxgkDdiControlInterrupt;

    PDXGKDDISETPOWERCOMPONENTFSTATE         DxgkDdiSetPowerComponentFState;
    PDXGKDDIPOWERRUNTIMECONTROLREQUEST      DxgkDdiPowerRuntimeControlRequest;

    //
    // New DDI for the surprise removal support.
    //
    PDXGKDDI_NOTIFY_SURPRISE_REMOVAL        DxgkDdiNotifySurpriseRemoval;
#endif
    //
    // Display only drivers support P-State management.
    //
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    PDXGKDDI_POWERRUNTIMESETDEVICEHANDLE    DxgkDdiPowerRuntimeSetDeviceHandle;
#endif
} KMDDOD_INITIALIZATION_DATA, *PKMDDOD_INITIALIZATION_DATA;

typedef struct _DRIVER_INITIALIZATION_DATA {
    ULONG                                   Version;
    //TODO: Fill these out with the pfns
} DRIVER_INITIALIZATION_DATA, *PDRIVER_INITIALIZATION_DATA;
