/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     System calls Windows 11 dxgkrnl implements, passed on to it unchanged
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

VOID
NTAPI
DlResolveDxgkForwards(
    _In_ PVOID ImageBase);

NTSTATUS
NTAPI
DlForwardSetHwProtectionTeardownRecovery(
    _Inout_ PVOID Arg);

/* The forwarders, which the system call table lists */
NTSTATUS
APIENTRY
NtBindCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4,
    _In_opt_ PVOID Param5,
    _In_opt_ PVOID Param6);

NTSTATUS
APIENTRY
NtConfirmCompositionSurfaceIndependentFlipEntry(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4,
    _In_ ULONG Param5);

NTSTATUS
APIENTRY
NtCreateCompositionSurfaceHandle(
    _In_ ULONG_PTR Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtDxgkAcquireExclusiveEngineAccess(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkCancelPresents(
    _In_ ULONG_PTR Param1);

NTSTATUS
APIENTRY
NtDxgkCheckEngineAccess(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkCheckSinglePlaneForMultiPlaneOverlaySupport(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkConnectDoorbell(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkCreateDoorbell(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkCreateNativeFence(
    _In_ ULONG_PTR Param1);

NTSTATUS
APIENTRY
NtDxgkCreateTrackedWorkload(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkDestroyDoorbell(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkDestroyTrackedWorkload(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkDisableProcessDebugBlobCollection(VOID);

NTSTATUS
APIENTRY
NtDxgkDisplayMuxSwitchExecute(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkDisplayMuxSwitchFinish(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkDisplayMuxSwitchPrepare(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkDisplayPortOperation(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkDuplicateHandle(
    _In_ ULONG Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkEnableProcessDebugBlobCollection(VOID);

NTSTATUS
APIENTRY
NtDxgkEnumAdapters3(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkEnumProcesses(
    _In_ ULONG Param1);

NTSTATUS
APIENTRY
NtDxgkGetAvailableTrackedWorkloadIndex(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkGetNativeFenceLogDetail(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkGetProcessList(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkGetProperties(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkGetTrackedWorkloadStatistics(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkIsFeatureEnabled(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkMapProcessDebugBlob(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkNotifyWorkSubmission(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkOpenNativeFenceFromNtHandle(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkOutputDuplPresentToHwQueue(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkPinResources(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkRegisterVailProcess(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkResetTrackedWorkloadStatistics(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkSetProperties(
    _In_ ULONG Param1);

NTSTATUS
APIENTRY
NtDxgkSubmitPresentBltToHwQueue(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkSubmitPresentToHwQueue(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkUnmapProcessDebugBlob(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkUnpinResources(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkUpdateTrackedWorkload(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtDxgkVailConnect(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkVailDisconnect(
    _In_ ULONG_PTR Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtDxgkVailPromoteCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG_PTR Param3);

NTSTATUS
APIENTRY
NtFlipObjectAddContent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4);

NTSTATUS
APIENTRY
NtFlipObjectAddPoolBuffer(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4,
    _In_ ULONG Param5,
    _In_ ULONG_PTR Param6);

NTSTATUS
APIENTRY
NtFlipObjectConsumerAcquirePresent(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4,
    _In_ ULONG_PTR Param5);

NTSTATUS
APIENTRY
NtFlipObjectConsumerAdjustUsageReference(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3);

NTSTATUS
APIENTRY
NtFlipObjectConsumerBeginProcessPresent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtFlipObjectConsumerEndProcessPresent(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtFlipObjectConsumerPostMessage(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtFlipObjectConsumerQueryBufferInfo(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4);

NTSTATUS
APIENTRY
NtFlipObjectCreate(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtFlipObjectDisconnectEndpoint(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2);

NTSTATUS
APIENTRY
NtFlipObjectEnablePresentStatisticsType(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3);

NTSTATUS
APIENTRY
NtFlipObjectOpen(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4);

NTSTATUS
APIENTRY
NtFlipObjectPresentCancel(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtFlipObjectQueryBufferAvailableEvent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtFlipObjectQueryEndpointConnected(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtFlipObjectQueryLostEvent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtFlipObjectQueryNextMessageToProducer(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtFlipObjectReadNextMessageToProducer(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtFlipObjectRemoveContent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtFlipObjectRemovePoolBuffer(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtFlipObjectSetContent(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_ ULONG Param4,
    _In_ ULONG_PTR Param5);

NTSTATUS
APIENTRY
NtGdiDdDDIAbandonSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIAcquireSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIAddSurfaceToSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDICheckMultiPlaneOverlaySupport(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateBundleObject(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateHwContext(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateOutputDupl(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateProtectedSession(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIDDisplayEnum(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyHwContext(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyOutputDupl(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyProtectedSession(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIDispMgrCreate(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIDispMgrSourceOperation(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIDispMgrTargetOperation(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIExtractBundleObject(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIFlushHeapTransitions(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetAllocationPriority(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetContextInProcessSchedulingPriority(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetDWMVerticalBlankEvent(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetMemoryBudgetTarget(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetMultiPlaneOverlayCaps(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetPostCompositionCaps(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetProcessDeviceRemovalSupport(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetProcessSchedulingPriorityBand(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetResourcePresentPrivateDriverData(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetSetSwapChainMetadata(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetSharedResourceAdapterLuid(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetSharedResourceAdapterLuidFlipManager(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetSwapChainSurfacePhysicalAddress(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetYieldPercentage(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDINetDispGetNextChunkInfo(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDINetDispQueryMiracastDisplayDeviceStatus(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDINetDispQueryMiracastDisplayDeviceSupport(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDINetDispStartMiracastDisplayDevice(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDINetDispStopMiracastDisplayDevice(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOfferAllocations(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenBundleObjectNtHandleFromName(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenKeyedMutexFromNtHandle(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenProtectedSessionFromNtHandle(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplGetFrameInfo(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplGetMetaData(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplGetPointerShapeData(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplPresent(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIOutputDuplReleaseFrame(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIPresentMultiPlaneOverlay(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIPresentRedirected(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryFSEBlock(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryProcessOfferInfo(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryProtectedSessionInfoFromNtHandle(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryProtectedSessionStatus(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryRemoteVidPnSourceFromGdiDisplayName(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIReclaimAllocations(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIReleaseSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIRemoveSurfaceFromSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetDodIndirectSwapchain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetFSEBlock(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetMemoryBudgetTarget(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetMonitorColorSpaceTransform(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetProcessDeviceRemovalSupport(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetProcessSchedulingPriorityBand(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetStablePowerState(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetStereoEnabled(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetSyncRefreshCountWaitTarget(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDISetYieldPercentage(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDITrimProcessCommitment(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIUnOrderedPresentSwapChain(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForVerticalBlankEvent2(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtNotifyPresentToCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtOpenCompositionSurfaceDirtyRegion(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4);

NTSTATUS
APIENTRY
NtOpenCompositionSurfaceRealizationInfo(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4);

NTSTATUS
APIENTRY
NtOpenCompositionSurfaceSectionInfo(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4);

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceBinding(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceFrameRate(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceHDRMetaData(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_ ULONG_PTR Param4,
    _In_opt_ PVOID Param5);

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceRenderingRealization(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceStatistics(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);

NTSTATUS
APIENTRY
NtSetCompositionSurfaceAnalogExclusive(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2);

NTSTATUS
APIENTRY
NtSetCompositionSurfaceBufferUsage(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4,
    _In_ ULONG Param5);

NTSTATUS
APIENTRY
NtSetCompositionSurfaceDirectFlipState(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONG Param3,
    _In_ ULONG Param4);

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
    _In_opt_ PVOID Param8);

NTSTATUS
APIENTRY
NtSetCompositionSurfaceStatistics(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3);

NTSTATUS
APIENTRY
NtTokenManagerConfirmOutstandingAnalogToken(VOID);

NTSTATUS
APIENTRY
NtTokenManagerCreateCompositionTokenHandle(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4,
    _In_opt_ PVOID Param5,
    _In_opt_ PVOID Param6);

NTSTATUS
APIENTRY
NtTokenManagerCreateFlipObjectReturnTokenHandle(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3);

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
    _In_opt_ PVOID Param9);

NTSTATUS
APIENTRY
NtTokenManagerGetAnalogExclusiveSurfaceUpdates(
    _In_ ULONG Param1,
    _In_ ULONG_PTR Param2,
    _In_ ULONG Param3,
    _In_opt_ PVOID Param4,
    _In_opt_ PVOID Param5);

NTSTATUS
APIENTRY
NtTokenManagerGetAnalogExclusiveTokenEvent(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtTokenManagerOpenSectionAndEvents(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_opt_ PVOID Param3,
    _In_opt_ PVOID Param4);

NTSTATUS
APIENTRY
NtTokenManagerThread(
    _In_opt_ PVOID Param1);

NTSTATUS
APIENTRY
NtUnBindCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ ULONG Param2);

NTSTATUS
APIENTRY
NtValidateCompositionSurfaceHandle(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2);
