/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     System calls Windows 11 dxgkrnl implements, passed on to it unchanged
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/rddm/rddm_private.h>
#include "dxgkfwd.h"
#include <debug.h>

/* Exported by ntoskrnl */
NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_ PCSTR RoutineName);

/*
 * Composition surfaces, flip objects, the token manager and the later D3DKMT calls live in
 * dxgkrnl itself, which exports each one under its system call name. win32k only looks them
 * up and hands the caller's arguments over; dxgkrnl probes and captures them.
 */
typedef enum _DXGK_FORWARD
{
    DxgkForward_BindCompositionSurface,
    DxgkForward_ConfirmCompositionSurfaceIndependentFlipEntry,
    DxgkForward_CreateCompositionSurfaceHandle,
    DxgkForward_DxgkAcquireExclusiveEngineAccess,
    DxgkForward_DxgkCancelPresents,
    DxgkForward_DxgkCheckEngineAccess,
    DxgkForward_DxgkCheckSinglePlaneForMultiPlaneOverlaySupport,
    DxgkForward_DxgkConnectDoorbell,
    DxgkForward_DxgkCreateDoorbell,
    DxgkForward_DxgkCreateNativeFence,
    DxgkForward_DxgkCreateTrackedWorkload,
    DxgkForward_DxgkDestroyDoorbell,
    DxgkForward_DxgkDestroyTrackedWorkload,
    DxgkForward_DxgkDisableProcessDebugBlobCollection,
    DxgkForward_DxgkDisplayMuxSwitchExecute,
    DxgkForward_DxgkDisplayMuxSwitchFinish,
    DxgkForward_DxgkDisplayMuxSwitchPrepare,
    DxgkForward_DxgkDisplayPortOperation,
    DxgkForward_DxgkDuplicateHandle,
    DxgkForward_DxgkEnableProcessDebugBlobCollection,
    DxgkForward_DxgkEnumAdapters3,
    DxgkForward_DxgkEnumProcesses,
    DxgkForward_DxgkGetAvailableTrackedWorkloadIndex,
    DxgkForward_DxgkGetNativeFenceLogDetail,
    DxgkForward_DxgkGetProcessList,
    DxgkForward_DxgkGetProperties,
    DxgkForward_DxgkGetTrackedWorkloadStatistics,
    DxgkForward_DxgkIsFeatureEnabled,
    DxgkForward_DxgkMapProcessDebugBlob,
    DxgkForward_DxgkNotifyWorkSubmission,
    DxgkForward_DxgkOpenNativeFenceFromNtHandle,
    DxgkForward_DxgkOutputDuplPresentToHwQueue,
    DxgkForward_DxgkPinResources,
    DxgkForward_DxgkRegisterVailProcess,
    DxgkForward_DxgkResetTrackedWorkloadStatistics,
    DxgkForward_DxgkSetProperties,
    DxgkForward_DxgkSubmitPresentBltToHwQueue,
    DxgkForward_DxgkSubmitPresentToHwQueue,
    DxgkForward_DxgkUnmapProcessDebugBlob,
    DxgkForward_DxgkUnpinResources,
    DxgkForward_DxgkUpdateTrackedWorkload,
    DxgkForward_DxgkVailConnect,
    DxgkForward_DxgkVailDisconnect,
    DxgkForward_DxgkVailPromoteCompositionSurface,
    DxgkForward_FlipObjectAddContent,
    DxgkForward_FlipObjectAddPoolBuffer,
    DxgkForward_FlipObjectConsumerAcquirePresent,
    DxgkForward_FlipObjectConsumerAdjustUsageReference,
    DxgkForward_FlipObjectConsumerBeginProcessPresent,
    DxgkForward_FlipObjectConsumerEndProcessPresent,
    DxgkForward_FlipObjectConsumerPostMessage,
    DxgkForward_FlipObjectConsumerQueryBufferInfo,
    DxgkForward_FlipObjectCreate,
    DxgkForward_FlipObjectDisconnectEndpoint,
    DxgkForward_FlipObjectEnablePresentStatisticsType,
    DxgkForward_FlipObjectOpen,
    DxgkForward_FlipObjectPresentCancel,
    DxgkForward_FlipObjectQueryBufferAvailableEvent,
    DxgkForward_FlipObjectQueryEndpointConnected,
    DxgkForward_FlipObjectQueryLostEvent,
    DxgkForward_FlipObjectQueryNextMessageToProducer,
    DxgkForward_FlipObjectReadNextMessageToProducer,
    DxgkForward_FlipObjectRemoveContent,
    DxgkForward_FlipObjectRemovePoolBuffer,
    DxgkForward_FlipObjectSetContent,
    DxgkForward_GdiDdDDIAbandonSwapChain,
    DxgkForward_GdiDdDDIAcquireSwapChain,
    DxgkForward_GdiDdDDIAddSurfaceToSwapChain,
    DxgkForward_GdiDdDDICheckMultiPlaneOverlaySupport,
    DxgkForward_GdiDdDDICreateBundleObject,
    DxgkForward_GdiDdDDICreateHwContext,
    DxgkForward_GdiDdDDICreateOutputDupl,
    DxgkForward_GdiDdDDICreateProtectedSession,
    DxgkForward_GdiDdDDICreateSwapChain,
    DxgkForward_GdiDdDDIDDisplayEnum,
    DxgkForward_GdiDdDDIDestroyHwContext,
    DxgkForward_GdiDdDDIDestroyOutputDupl,
    DxgkForward_GdiDdDDIDestroyProtectedSession,
    DxgkForward_GdiDdDDIDispMgrCreate,
    DxgkForward_GdiDdDDIDispMgrSourceOperation,
    DxgkForward_GdiDdDDIDispMgrTargetOperation,
    DxgkForward_GdiDdDDIExtractBundleObject,
    DxgkForward_GdiDdDDIFlushHeapTransitions,
    DxgkForward_GdiDdDDIGetAllocationPriority,
    DxgkForward_GdiDdDDIGetContextInProcessSchedulingPriority,
    DxgkForward_GdiDdDDIGetDWMVerticalBlankEvent,
    DxgkForward_GdiDdDDIGetMemoryBudgetTarget,
    DxgkForward_GdiDdDDIGetMultiPlaneOverlayCaps,
    DxgkForward_GdiDdDDIGetPostCompositionCaps,
    DxgkForward_GdiDdDDIGetProcessDeviceRemovalSupport,
    DxgkForward_GdiDdDDIGetProcessSchedulingPriorityBand,
    DxgkForward_GdiDdDDIGetResourcePresentPrivateDriverData,
    DxgkForward_GdiDdDDIGetSetSwapChainMetadata,
    DxgkForward_GdiDdDDIGetSharedResourceAdapterLuid,
    DxgkForward_GdiDdDDIGetSharedResourceAdapterLuidFlipManager,
    DxgkForward_GdiDdDDIGetSwapChainSurfacePhysicalAddress,
    DxgkForward_GdiDdDDIGetYieldPercentage,
    DxgkForward_GdiDdDDINetDispGetNextChunkInfo,
    DxgkForward_GdiDdDDINetDispQueryMiracastDisplayDeviceStatus,
    DxgkForward_GdiDdDDINetDispQueryMiracastDisplayDeviceSupport,
    DxgkForward_GdiDdDDINetDispStartMiracastDisplayDevice,
    DxgkForward_GdiDdDDINetDispStopMiracastDisplayDevice,
    DxgkForward_GdiDdDDIOfferAllocations,
    DxgkForward_GdiDdDDIOpenBundleObjectNtHandleFromName,
    DxgkForward_GdiDdDDIOpenKeyedMutexFromNtHandle,
    DxgkForward_GdiDdDDIOpenProtectedSessionFromNtHandle,
    DxgkForward_GdiDdDDIOpenSwapChain,
    DxgkForward_GdiDdDDIOutputDuplGetFrameInfo,
    DxgkForward_GdiDdDDIOutputDuplGetMetaData,
    DxgkForward_GdiDdDDIOutputDuplGetPointerShapeData,
    DxgkForward_GdiDdDDIOutputDuplPresent,
    DxgkForward_GdiDdDDIOutputDuplReleaseFrame,
    DxgkForward_GdiDdDDIPresentMultiPlaneOverlay,
    DxgkForward_GdiDdDDIPresentRedirected,
    DxgkForward_GdiDdDDIQueryFSEBlock,
    DxgkForward_GdiDdDDIQueryProcessOfferInfo,
    DxgkForward_GdiDdDDIQueryProtectedSessionInfoFromNtHandle,
    DxgkForward_GdiDdDDIQueryProtectedSessionStatus,
    DxgkForward_GdiDdDDIQueryRemoteVidPnSourceFromGdiDisplayName,
    DxgkForward_GdiDdDDIReclaimAllocations,
    DxgkForward_GdiDdDDIReleaseSwapChain,
    DxgkForward_GdiDdDDIRemoveSurfaceFromSwapChain,
    DxgkForward_GdiDdDDISetDodIndirectSwapchain,
    DxgkForward_GdiDdDDISetFSEBlock,
    DxgkForward_GdiDdDDISetMemoryBudgetTarget,
    DxgkForward_GdiDdDDISetMonitorColorSpaceTransform,
    DxgkForward_GdiDdDDISetProcessDeviceRemovalSupport,
    DxgkForward_GdiDdDDISetProcessSchedulingPriorityBand,
    DxgkForward_GdiDdDDISetStablePowerState,
    DxgkForward_GdiDdDDISetStereoEnabled,
    DxgkForward_GdiDdDDISetSyncRefreshCountWaitTarget,
    DxgkForward_GdiDdDDISetYieldPercentage,
    DxgkForward_GdiDdDDITrimProcessCommitment,
    DxgkForward_GdiDdDDIUnOrderedPresentSwapChain,
    DxgkForward_GdiDdDDIWaitForVerticalBlankEvent2,
    DxgkForward_NotifyPresentToCompositionSurface,
    DxgkForward_OpenCompositionSurfaceDirtyRegion,
    DxgkForward_OpenCompositionSurfaceRealizationInfo,
    DxgkForward_OpenCompositionSurfaceSectionInfo,
    DxgkForward_QueryCompositionSurfaceBinding,
    DxgkForward_QueryCompositionSurfaceFrameRate,
    DxgkForward_QueryCompositionSurfaceHDRMetaData,
    DxgkForward_QueryCompositionSurfaceRenderingRealization,
    DxgkForward_QueryCompositionSurfaceStatistics,
    DxgkForward_SetCompositionSurfaceAnalogExclusive,
    DxgkForward_SetCompositionSurfaceBufferUsage,
    DxgkForward_SetCompositionSurfaceDirectFlipState,
    DxgkForward_SetCompositionSurfaceIndependentFlipInfo,
    DxgkForward_SetCompositionSurfaceStatistics,
    DxgkForward_TokenManagerConfirmOutstandingAnalogToken,
    DxgkForward_TokenManagerCreateCompositionTokenHandle,
    DxgkForward_TokenManagerCreateFlipObjectReturnTokenHandle,
    DxgkForward_TokenManagerCreateFlipObjectTokenHandle,
    DxgkForward_TokenManagerGetAnalogExclusiveSurfaceUpdates,
    DxgkForward_TokenManagerGetAnalogExclusiveTokenEvent,
    DxgkForward_TokenManagerOpenSectionAndEvents,
    DxgkForward_TokenManagerThread,
    DxgkForward_UnBindCompositionSurface,
    DxgkForward_ValidateCompositionSurfaceHandle,
    DxgkForward_GdiDdDDISetHwProtectionTeardownRecovery,
    DxgkForwardCount
} DXGK_FORWARD;

static const PCSTR DlpDxgkForwardNames[DxgkForwardCount] =
{
    "NtBindCompositionSurface",
    "NtConfirmCompositionSurfaceIndependentFlipEntry",
    "NtCreateCompositionSurfaceHandle",
    "NtDxgkAcquireExclusiveEngineAccess",
    "NtDxgkCancelPresents",
    "NtDxgkCheckEngineAccess",
    "NtDxgkCheckSinglePlaneForMultiPlaneOverlaySupport",
    "NtDxgkConnectDoorbell",
    "NtDxgkCreateDoorbell",
    "NtDxgkCreateNativeFence",
    "NtDxgkCreateTrackedWorkload",
    "NtDxgkDestroyDoorbell",
    "NtDxgkDestroyTrackedWorkload",
    "NtDxgkDisableProcessDebugBlobCollection",
    "NtDxgkDisplayMuxSwitchExecute",
    "NtDxgkDisplayMuxSwitchFinish",
    "NtDxgkDisplayMuxSwitchPrepare",
    "NtDxgkDisplayPortOperation",
    "NtDxgkDuplicateHandle",
    "NtDxgkEnableProcessDebugBlobCollection",
    "NtDxgkEnumAdapters3",
    "NtDxgkEnumProcesses",
    "NtDxgkGetAvailableTrackedWorkloadIndex",
    "NtDxgkGetNativeFenceLogDetail",
    "NtDxgkGetProcessList",
    "NtDxgkGetProperties",
    "NtDxgkGetTrackedWorkloadStatistics",
    "NtDxgkIsFeatureEnabled",
    "NtDxgkMapProcessDebugBlob",
    "NtDxgkNotifyWorkSubmission",
    "NtDxgkOpenNativeFenceFromNtHandle",
    "NtDxgkOutputDuplPresentToHwQueue",
    "NtDxgkPinResources",
    "NtDxgkRegisterVailProcess",
    "NtDxgkResetTrackedWorkloadStatistics",
    "NtDxgkSetProperties",
    "NtDxgkSubmitPresentBltToHwQueue",
    "NtDxgkSubmitPresentToHwQueue",
    "NtDxgkUnmapProcessDebugBlob",
    "NtDxgkUnpinResources",
    "NtDxgkUpdateTrackedWorkload",
    "NtDxgkVailConnect",
    "NtDxgkVailDisconnect",
    "NtDxgkVailPromoteCompositionSurface",
    "NtFlipObjectAddContent",
    "NtFlipObjectAddPoolBuffer",
    "NtFlipObjectConsumerAcquirePresent",
    "NtFlipObjectConsumerAdjustUsageReference",
    "NtFlipObjectConsumerBeginProcessPresent",
    "NtFlipObjectConsumerEndProcessPresent",
    "NtFlipObjectConsumerPostMessage",
    "NtFlipObjectConsumerQueryBufferInfo",
    "NtFlipObjectCreate",
    "NtFlipObjectDisconnectEndpoint",
    "NtFlipObjectEnablePresentStatisticsType",
    "NtFlipObjectOpen",
    "NtFlipObjectPresentCancel",
    "NtFlipObjectQueryBufferAvailableEvent",
    "NtFlipObjectQueryEndpointConnected",
    "NtFlipObjectQueryLostEvent",
    "NtFlipObjectQueryNextMessageToProducer",
    "NtFlipObjectReadNextMessageToProducer",
    "NtFlipObjectRemoveContent",
    "NtFlipObjectRemovePoolBuffer",
    "NtFlipObjectSetContent",
    "NtGdiDdDDIAbandonSwapChain",
    "NtGdiDdDDIAcquireSwapChain",
    "NtGdiDdDDIAddSurfaceToSwapChain",
    "NtGdiDdDDICheckMultiPlaneOverlaySupport",
    "NtGdiDdDDICreateBundleObject",
    "NtGdiDdDDICreateHwContext",
    "NtGdiDdDDICreateOutputDupl",
    "NtGdiDdDDICreateProtectedSession",
    "NtGdiDdDDICreateSwapChain",
    "NtGdiDdDDIDDisplayEnum",
    "NtGdiDdDDIDestroyHwContext",
    "NtGdiDdDDIDestroyOutputDupl",
    "NtGdiDdDDIDestroyProtectedSession",
    "NtGdiDdDDIDispMgrCreate",
    "NtGdiDdDDIDispMgrSourceOperation",
    "NtGdiDdDDIDispMgrTargetOperation",
    "NtGdiDdDDIExtractBundleObject",
    "NtGdiDdDDIFlushHeapTransitions",
    "NtGdiDdDDIGetAllocationPriority",
    "NtGdiDdDDIGetContextInProcessSchedulingPriority",
    "NtGdiDdDDIGetDWMVerticalBlankEvent",
    "NtGdiDdDDIGetMemoryBudgetTarget",
    "NtGdiDdDDIGetMultiPlaneOverlayCaps",
    "NtGdiDdDDIGetPostCompositionCaps",
    "NtGdiDdDDIGetProcessDeviceRemovalSupport",
    "NtGdiDdDDIGetProcessSchedulingPriorityBand",
    "NtGdiDdDDIGetResourcePresentPrivateDriverData",
    "NtGdiDdDDIGetSetSwapChainMetadata",
    "NtGdiDdDDIGetSharedResourceAdapterLuid",
    "NtGdiDdDDIGetSharedResourceAdapterLuidFlipManager",
    "NtGdiDdDDIGetSwapChainSurfacePhysicalAddress",
    "NtGdiDdDDIGetYieldPercentage",
    "NtGdiDdDDINetDispGetNextChunkInfo",
    "NtGdiDdDDINetDispQueryMiracastDisplayDeviceStatus",
    "NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport",
    "NtGdiDdDDINetDispStartMiracastDisplayDevice",
    "NtGdiDdDDINetDispStopMiracastDisplayDevice",
    "NtGdiDdDDIOfferAllocations",
    "NtGdiDdDDIOpenBundleObjectNtHandleFromName",
    "NtGdiDdDDIOpenKeyedMutexFromNtHandle",
    "NtGdiDdDDIOpenProtectedSessionFromNtHandle",
    "NtGdiDdDDIOpenSwapChain",
    "NtGdiDdDDIOutputDuplGetFrameInfo",
    "NtGdiDdDDIOutputDuplGetMetaData",
    "NtGdiDdDDIOutputDuplGetPointerShapeData",
    "NtGdiDdDDIOutputDuplPresent",
    "NtGdiDdDDIOutputDuplReleaseFrame",
    "NtGdiDdDDIPresentMultiPlaneOverlay",
    "NtGdiDdDDIPresentRedirected",
    "NtGdiDdDDIQueryFSEBlock",
    "NtGdiDdDDIQueryProcessOfferInfo",
    "NtGdiDdDDIQueryProtectedSessionInfoFromNtHandle",
    "NtGdiDdDDIQueryProtectedSessionStatus",
    "NtGdiDdDDIQueryRemoteVidPnSourceFromGdiDisplayName",
    "NtGdiDdDDIReclaimAllocations",
    "NtGdiDdDDIReleaseSwapChain",
    "NtGdiDdDDIRemoveSurfaceFromSwapChain",
    "NtGdiDdDDISetDodIndirectSwapchain",
    "NtGdiDdDDISetFSEBlock",
    "NtGdiDdDDISetMemoryBudgetTarget",
    "NtGdiDdDDISetMonitorColorSpaceTransform",
    "NtGdiDdDDISetProcessDeviceRemovalSupport",
    "NtGdiDdDDISetProcessSchedulingPriorityBand",
    "NtGdiDdDDISetStablePowerState",
    "NtGdiDdDDISetStereoEnabled",
    "NtGdiDdDDISetSyncRefreshCountWaitTarget",
    "NtGdiDdDDISetYieldPercentage",
    "NtGdiDdDDITrimProcessCommitment",
    "NtGdiDdDDIUnOrderedPresentSwapChain",
    "NtGdiDdDDIWaitForVerticalBlankEvent2",
    "NtNotifyPresentToCompositionSurface",
    "NtOpenCompositionSurfaceDirtyRegion",
    "NtOpenCompositionSurfaceRealizationInfo",
    "NtOpenCompositionSurfaceSectionInfo",
    "NtQueryCompositionSurfaceBinding",
    "NtQueryCompositionSurfaceFrameRate",
    "NtQueryCompositionSurfaceHDRMetaData",
    "NtQueryCompositionSurfaceRenderingRealization",
    "NtQueryCompositionSurfaceStatistics",
    "NtSetCompositionSurfaceAnalogExclusive",
    "NtSetCompositionSurfaceBufferUsage",
    "NtSetCompositionSurfaceDirectFlipState",
    "NtSetCompositionSurfaceIndependentFlipInfo",
    "NtSetCompositionSurfaceStatistics",
    "NtTokenManagerConfirmOutstandingAnalogToken",
    "NtTokenManagerCreateCompositionTokenHandle",
    "NtTokenManagerCreateFlipObjectReturnTokenHandle",
    "NtTokenManagerCreateFlipObjectTokenHandle",
    "NtTokenManagerGetAnalogExclusiveSurfaceUpdates",
    "NtTokenManagerGetAnalogExclusiveTokenEvent",
    "NtTokenManagerOpenSectionAndEvents",
    "NtTokenManagerThread",
    "NtUnBindCompositionSurface",
    "NtValidateCompositionSurfaceHandle",
    "NtGdiDdDDISetHwProtectionTeardownRecovery",
};

static PVOID DlpDxgkForwards[DxgkForwardCount];

/**
 * @brief Finds every forwarded entry point among the exports of the loaded dxgkrnl.
 */
VOID
NTAPI
DlResolveDxgkForwards(
    _In_ PVOID ImageBase)
{
    ULONG i;

    for (i = 0; i < DxgkForwardCount; i++)
        DlpDxgkForwards[i] = RtlFindExportedRoutineByName(ImageBase, DlpDxgkForwardNames[i]);
}

static
PVOID
DlpGetDxgkForward(
    _In_ DXGK_FORWARD Index)
{
    if (!DlpDxgkForwards[Index])
        DPRINT("win32k: dxgkrnl does not export %s\n", DlpDxgkForwardNames[Index]);
    return DlpDxgkForwards[Index];
}

NTSTATUS
APIENTRY
NtBindCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4,
    _In_opt_ PVOID Param5,
    _In_opt_ PVOID Param6)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, ULONG, ULONG, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_BindCompositionSurface);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5, Param6);
}

NTSTATUS
APIENTRY
NtConfirmCompositionSurfaceIndependentFlipEntry(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4,
    _In_ ULONG Param5)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, ULONG, ULONG, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_ConfirmCompositionSurfaceIndependentFlipEntry);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5);
}

NTSTATUS
APIENTRY
NtCreateCompositionSurfaceHandle(
    _In_ ULONG_PTR Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_CreateCompositionSurfaceHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkAcquireExclusiveEngineAccess(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkAcquireExclusiveEngineAccess);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkCancelPresents(
    _In_ ULONG_PTR Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkCancelPresents);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkCheckEngineAccess(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkCheckEngineAccess);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkCheckSinglePlaneForMultiPlaneOverlaySupport(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkCheckSinglePlaneForMultiPlaneOverlaySupport);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkConnectDoorbell(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkConnectDoorbell);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkCreateDoorbell(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkCreateDoorbell);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkCreateNativeFence(
    _In_ ULONG_PTR Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkCreateNativeFence);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkCreateTrackedWorkload(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkCreateTrackedWorkload);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkDestroyDoorbell(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDestroyDoorbell);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkDestroyTrackedWorkload(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDestroyTrackedWorkload);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkDisableProcessDebugBlobCollection(VOID)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(VOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDisableProcessDebugBlobCollection);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn();
}

NTSTATUS
APIENTRY
NtDxgkDisplayMuxSwitchExecute(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDisplayMuxSwitchExecute);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkDisplayMuxSwitchFinish(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDisplayMuxSwitchFinish);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkDisplayMuxSwitchPrepare(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDisplayMuxSwitchPrepare);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkDisplayPortOperation(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDisplayPortOperation);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkDuplicateHandle(
    _In_ ULONG Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkDuplicateHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkEnableProcessDebugBlobCollection(VOID)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(VOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkEnableProcessDebugBlobCollection);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn();
}

NTSTATUS
APIENTRY
NtDxgkEnumAdapters3(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkEnumAdapters3);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkEnumProcesses(
    _In_ ULONG Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkEnumProcesses);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkGetAvailableTrackedWorkloadIndex(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkGetAvailableTrackedWorkloadIndex);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkGetNativeFenceLogDetail(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkGetNativeFenceLogDetail);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkGetProcessList(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkGetProcessList);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkGetProperties(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkGetProperties);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkGetTrackedWorkloadStatistics(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkGetTrackedWorkloadStatistics);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkIsFeatureEnabled(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkIsFeatureEnabled);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkMapProcessDebugBlob(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkMapProcessDebugBlob);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkNotifyWorkSubmission(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkNotifyWorkSubmission);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkOpenNativeFenceFromNtHandle(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkOpenNativeFenceFromNtHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkOutputDuplPresentToHwQueue(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkOutputDuplPresentToHwQueue);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkPinResources(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkPinResources);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkRegisterVailProcess(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkRegisterVailProcess);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkResetTrackedWorkloadStatistics(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkResetTrackedWorkloadStatistics);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkSetProperties(
    _In_ ULONG Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkSetProperties);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkSubmitPresentBltToHwQueue(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkSubmitPresentBltToHwQueue);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkSubmitPresentToHwQueue(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkSubmitPresentToHwQueue);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkUnmapProcessDebugBlob(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkUnmapProcessDebugBlob);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkUnpinResources(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkUnpinResources);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkUpdateTrackedWorkload(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkUpdateTrackedWorkload);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtDxgkVailConnect(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkVailConnect);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkVailDisconnect(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkVailDisconnect);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtDxgkVailPromoteCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG_PTR, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_DxgkVailPromoteCompositionSurface);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectAddContent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, ULONG, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectAddContent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtFlipObjectAddPoolBuffer(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4,
    _In_ ULONG Param5,
    _In_ ULONG_PTR Param6)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, PVOID, ULONG, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectAddPoolBuffer);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5, Param6);
}

NTSTATUS
APIENTRY
NtFlipObjectConsumerAcquirePresent(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4,
    _In_ ULONG_PTR Param5)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, ULONG, PVOID, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectConsumerAcquirePresent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5);
}

NTSTATUS
APIENTRY
NtFlipObjectConsumerAdjustUsageReference(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectConsumerAdjustUsageReference);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectConsumerBeginProcessPresent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectConsumerBeginProcessPresent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectConsumerEndProcessPresent(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectConsumerEndProcessPresent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtFlipObjectConsumerPostMessage(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectConsumerPostMessage);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectConsumerQueryBufferInfo(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectConsumerQueryBufferInfo);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtFlipObjectCreate(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectCreate);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtFlipObjectDisconnectEndpoint(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectDisconnectEndpoint);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtFlipObjectEnablePresentStatisticsType(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectEnablePresentStatisticsType);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectOpen(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectOpen);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtFlipObjectPresentCancel(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectPresentCancel);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtFlipObjectQueryBufferAvailableEvent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectQueryBufferAvailableEvent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectQueryEndpointConnected(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectQueryEndpointConnected);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectQueryLostEvent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectQueryLostEvent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtFlipObjectQueryNextMessageToProducer(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectQueryNextMessageToProducer);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtFlipObjectReadNextMessageToProducer(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectReadNextMessageToProducer);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtFlipObjectRemoveContent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectRemoveContent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtFlipObjectRemovePoolBuffer(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectRemovePoolBuffer);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtFlipObjectSetContent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_ ULONG Param4,
    _In_ ULONG_PTR Param5)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, ULONG, ULONG_PTR);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_FlipObjectSetContent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5);
}

NTSTATUS
APIENTRY
NtGdiDdDDIAbandonSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIAbandonSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIAcquireSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIAcquireSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIAddSurfaceToSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIAddSurfaceToSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICheckMultiPlaneOverlaySupport(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDICheckMultiPlaneOverlaySupport);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateBundleObject(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDICreateBundleObject);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateHwContext(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDICreateHwContext);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateOutputDupl(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDICreateOutputDupl);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateProtectedSession(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDICreateProtectedSession);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDICreateSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDDisplayEnum(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIDDisplayEnum);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyHwContext(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIDestroyHwContext);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyOutputDupl(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIDestroyOutputDupl);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyProtectedSession(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIDestroyProtectedSession);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDispMgrCreate(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIDispMgrCreate);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDispMgrSourceOperation(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIDispMgrSourceOperation);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDispMgrTargetOperation(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIDispMgrTargetOperation);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIExtractBundleObject(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIExtractBundleObject);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIFlushHeapTransitions(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIFlushHeapTransitions);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetAllocationPriority(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetAllocationPriority);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetContextInProcessSchedulingPriority(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetContextInProcessSchedulingPriority);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetDWMVerticalBlankEvent(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetDWMVerticalBlankEvent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetMemoryBudgetTarget(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetMemoryBudgetTarget);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetMultiPlaneOverlayCaps(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetMultiPlaneOverlayCaps);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetPostCompositionCaps(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetPostCompositionCaps);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetProcessDeviceRemovalSupport(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetProcessDeviceRemovalSupport);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetProcessSchedulingPriorityBand(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetProcessSchedulingPriorityBand);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetResourcePresentPrivateDriverData(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetResourcePresentPrivateDriverData);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetSetSwapChainMetadata(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetSetSwapChainMetadata);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetSharedResourceAdapterLuid(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetSharedResourceAdapterLuid);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetSharedResourceAdapterLuidFlipManager(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetSharedResourceAdapterLuidFlipManager);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetSwapChainSurfacePhysicalAddress(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetSwapChainSurfacePhysicalAddress);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetYieldPercentage(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIGetYieldPercentage);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDINetDispGetNextChunkInfo(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDINetDispGetNextChunkInfo);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDINetDispQueryMiracastDisplayDeviceStatus(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDINetDispQueryMiracastDisplayDeviceStatus);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDINetDispQueryMiracastDisplayDeviceSupport);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDINetDispStartMiracastDisplayDevice(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDINetDispStartMiracastDisplayDevice);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDINetDispStopMiracastDisplayDevice(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDINetDispStopMiracastDisplayDevice);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOfferAllocations(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOfferAllocations);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenBundleObjectNtHandleFromName(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOpenBundleObjectNtHandleFromName);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenKeyedMutexFromNtHandle(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOpenKeyedMutexFromNtHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenProtectedSessionFromNtHandle(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOpenProtectedSessionFromNtHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOpenSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplGetFrameInfo(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOutputDuplGetFrameInfo);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplGetMetaData(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOutputDuplGetMetaData);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplGetPointerShapeData(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOutputDuplGetPointerShapeData);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplPresent(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOutputDuplPresent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplReleaseFrame(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIOutputDuplReleaseFrame);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIPresentMultiPlaneOverlay(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIPresentMultiPlaneOverlay);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIPresentRedirected(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIPresentRedirected);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryFSEBlock(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIQueryFSEBlock);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryProcessOfferInfo(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIQueryProcessOfferInfo);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryProtectedSessionInfoFromNtHandle(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIQueryProtectedSessionInfoFromNtHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryProtectedSessionStatus(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIQueryProtectedSessionStatus);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryRemoteVidPnSourceFromGdiDisplayName(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIQueryRemoteVidPnSourceFromGdiDisplayName);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIReclaimAllocations(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIReclaimAllocations);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIReleaseSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIReleaseSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIRemoveSurfaceFromSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIRemoveSurfaceFromSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetDodIndirectSwapchain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetDodIndirectSwapchain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetFSEBlock(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetFSEBlock);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetMemoryBudgetTarget(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetMemoryBudgetTarget);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetMonitorColorSpaceTransform(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetMonitorColorSpaceTransform);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetProcessDeviceRemovalSupport(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetProcessDeviceRemovalSupport);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetProcessSchedulingPriorityBand(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetProcessSchedulingPriorityBand);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetStablePowerState(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetStablePowerState);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetStereoEnabled(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetStereoEnabled);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetSyncRefreshCountWaitTarget(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetSyncRefreshCountWaitTarget);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetYieldPercentage(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetYieldPercentage);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDITrimProcessCommitment(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDITrimProcessCommitment);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIUnOrderedPresentSwapChain(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIUnOrderedPresentSwapChain);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForVerticalBlankEvent2(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDIWaitForVerticalBlankEvent2);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtNotifyPresentToCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_NotifyPresentToCompositionSurface);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtOpenCompositionSurfaceDirtyRegion(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_OpenCompositionSurfaceDirtyRegion);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtOpenCompositionSurfaceRealizationInfo(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_OpenCompositionSurfaceRealizationInfo);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtOpenCompositionSurfaceSectionInfo(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_OpenCompositionSurfaceSectionInfo);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceBinding(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_QueryCompositionSurfaceBinding);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceFrameRate(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_QueryCompositionSurfaceFrameRate);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceHDRMetaData(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_ ULONG_PTR Param4,
    _In_opt_ PVOID Param5)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, ULONG_PTR, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_QueryCompositionSurfaceHDRMetaData);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5);
}

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceRenderingRealization(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_QueryCompositionSurfaceRenderingRealization);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceStatistics(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_QueryCompositionSurfaceStatistics);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtSetCompositionSurfaceAnalogExclusive(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_SetCompositionSurfaceAnalogExclusive);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtSetCompositionSurfaceBufferUsage(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4,
    _In_ ULONG Param5)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, ULONG, ULONG, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_SetCompositionSurfaceBufferUsage);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5);
}

NTSTATUS
APIENTRY
NtSetCompositionSurfaceDirectFlipState(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, ULONG, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_SetCompositionSurfaceDirectFlipState);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtSetCompositionSurfaceIndependentFlipInfo(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4,
    _In_ ULONG Param5,
    _In_ ULONG Param6,
    _In_ ULONG_PTR Param7,
    _In_opt_ PVOID Param8)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, ULONG, ULONG, ULONG, ULONG, ULONG_PTR, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_SetCompositionSurfaceIndependentFlipInfo);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5, Param6, Param7, Param8);
}

NTSTATUS
APIENTRY
NtSetCompositionSurfaceStatistics(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_SetCompositionSurfaceStatistics);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtTokenManagerConfirmOutstandingAnalogToken(VOID)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(VOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerConfirmOutstandingAnalogToken);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn();
}

NTSTATUS
APIENTRY
NtTokenManagerCreateCompositionTokenHandle(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4,
    _In_opt_ PVOID Param5,
    _In_opt_ PVOID Param6)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG, ULONG, PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerCreateCompositionTokenHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5, Param6);
}

NTSTATUS
APIENTRY
NtTokenManagerCreateFlipObjectReturnTokenHandle(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerCreateFlipObjectReturnTokenHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3);
}

NTSTATUS
APIENTRY
NtTokenManagerCreateFlipObjectTokenHandle(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_ ULONG Param4,
    _In_ ULONG Param5,
    _In_opt_ PVOID Param6,
    _In_opt_ PVOID Param7,
    _In_opt_ PVOID Param8,
    _In_opt_ PVOID Param9)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, ULONG, ULONG, PVOID, PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerCreateFlipObjectTokenHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5, Param6, Param7, Param8, Param9);
}

NTSTATUS
APIENTRY
NtTokenManagerGetAnalogExclusiveSurfaceUpdates(
    _In_ ULONG Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4,
    _In_opt_ PVOID Param5)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(ULONG, ULONG_PTR, ULONG, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerGetAnalogExclusiveSurfaceUpdates);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4, Param5);
}

NTSTATUS
APIENTRY
NtTokenManagerGetAnalogExclusiveTokenEvent(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerGetAnalogExclusiveTokenEvent);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtTokenManagerOpenSectionAndEvents(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID, PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerOpenSectionAndEvents);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2, Param3, Param4);
}

NTSTATUS
APIENTRY
NtTokenManagerThread(
    _In_opt_ PVOID Param1)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_TokenManagerThread);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1);
}

NTSTATUS
APIENTRY
NtUnBindCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, ULONG);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_UnBindCompositionSurface);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

NTSTATUS
APIENTRY
NtValidateCompositionSurfaceHandle(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID, PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_ValidateCompositionSurfaceHandle);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Param1, Param2);
}

/* Entry points with an older ReactOS system call that now reach their dxgkrnl export */
NTSTATUS
NTAPI
DlForwardSetHwProtectionTeardownRecovery(
    _Inout_ PVOID Arg)
{
    typedef NTSTATUS (APIENTRY *PFN_FORWARD)(PVOID);
    PFN_FORWARD pfn = (PFN_FORWARD)DlpGetDxgkForward(DxgkForward_GdiDdDDISetHwProtectionTeardownRecovery);

    if (!pfn)
        return STATUS_PROCEDURE_NOT_FOUND;
    return pfn(Arg);
}
