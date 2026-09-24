/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Header file for RDDM Undocumented shared info
 * COPYRIGHT:   Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * Create an IO request to fill out the function pointer list.
 *
 * A WDDM miniport's DriverEntry calls DxgkInitialize, which is not an export but an inline stub in
 * the WDK's dispmprt.h: it opens \Device\DxgKrnl, sends one of these codes, and calls back through
 * the returned function pointer. WHICH code it sends depends on the WDK the miniport was built
 * with, not on the WDDM version it implements - so all three must be answered or the driver's
 * DriverEntry fails outright (Reference dxgkrnl.c:179719-179740).
 *
 *   0x23003F -> DpiInitialize        (original DxgkInitialize)
 *   0x230043 -> DpiKmdDodInitialize  (DxgkInitializeDisplayOnlyDriver)
 *   0x230047 -> DpiInitializeWin8    (DxgkInitialize on Win8+ WDKs)
 */
#define IOCTL_VIDEO_DDI_FUNC_REGISTER \
	CTL_CODE( FILE_DEVICE_VIDEO, 0xF, METHOD_NEITHER, FILE_ANY_ACCESS  )

/* EXACT value 0x230043 - DxgkInitializeDisplayOnlyDriver (Reference dxgkrnl.c:179726). */
#define IOCTL_VIDEO_DDI_FUNC_REGISTER_KMDDOD \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x10, METHOD_NEITHER, FILE_ANY_ACCESS  )

/* EXACT value 0x230047 - DxgkInitialize as emitted by Win8 and later WDKs (Reference :179733). */
#define IOCTL_VIDEO_DDI_FUNC_REGISTER_WIN8 \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x11, METHOD_NEITHER, FILE_ANY_ACCESS  )

/*
 * win32k passes a PDXGKWIN32K_INTERFACE (rxgkwddminterface.h, Version 22, 944 bytes) in
 * Irp->UserBuffer; dxgkrnl fills its pfnDxgk* slots with the D3DKMT entry points. METHOD_NEITHER:
 * the buffer is the caller's. EXACT value 0x23E057 - the code win32k sends (Reference
 * win32kbase.c:110324, DlInitDxgkrnl).
 */
#define IOCTL_VIDEO_GIVE_CALLSBACK \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x815, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA )

/*
 * The CDD (cdd.dll) passes a PDXGKCDD_INTERFACE (rxgkcdd.h) in Irp->UserBuffer; dxgkrnl fills the
 * CDD entry points (DxgkCddQueryInterface) so GDI can drive the desktop primary onto the screen.
 * EXACT value 0x23E05B - the code the decompiled CDD sends (Reference cdd.c OpenDxgkrnl:1463).
 */
#define IOCTL_VIDEO_QUERY_CDD_INTERFACE \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x816, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA )

/*
 * ReactOS-specific: win32k passes a REACTOS_WIN32K_DXGKRNL_INTERFACE (rxgkinterface.h) in
 * Irp->UserBuffer; dxgkrnl fills its RxgkIntPfn* slots with the D3DKMT entry points so ReactOS
 * win32k's existing NtGdiDdDDI* thunks (gdi/ntgdi/d3dkmt.c) reach DxgKrnl_ms. Not a Windows IOCTL.
 */
#define IOCTL_VIDEO_REGISTER_RXGK \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x817, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA )

/*
 * How win32k registers VideoPortCallout with \Device\VideoN and learns the adapter's physical
 * device object: one VIDEO_WIN32K_CALLBACKS serves as input and output, sent as an internal device
 * control. dxgkrnl and Windows' videoprt answer only this, not the public
 * IOCTL_VIDEO_INIT_WIN32K_CALLBACKS. EXACT value 0x23201F (Reference win32kbase.c:58330,
 * dxgkrnl.c:88719).
 */
#define IOCTL_VIDEO_GDI_INIT_WIN32K_CALLBACKS \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x807, METHOD_NEITHER, FILE_ANY_ACCESS )

/*
 * win32k sends this to \Device\VideoN (INTERNAL_DEVICE_CONTROL, buffer in Irp->UserBuffer).
 * dxgkrnl answers it for a WDDM adapter, and win32k drives those with the CDD instead of the
 * display driver named by InstalledDisplayDrivers (Reference win32kbase.c:58372,
 * DrvUpdateGraphicsDeviceList).
 */
#define IOCTL_VIDEO_QUERY_GDI_VIEW_INFORMATION \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x80C, METHOD_NEITHER, FILE_ANY_ACCESS )

typedef struct _DXGK_GDI_VIEW_INFORMATION
{
    ULONG Type;             /* 2 on a WDDM adapter */
    ULONG VidPnSourceId;
    PVOID Adapter;          /* NULL when nothing WDDM drives this device */
    LUID AdapterLuid;
} DXGK_GDI_VIEW_INFORMATION, *PDXGK_GDI_VIEW_INFORMATION;

/*
 * Claims a display device for the calling session, which is what gives the session its view of
 * the adapter. Without it the CDD finds no session adapter and cannot create its device
 * (Reference win32kbase.c:38559, bSetDeviceSessionUsage). The same buffer carries the answer.
 */
#define IOCTL_VIDEO_SET_SESSION_USAGE \
	CTL_CODE( FILE_DEVICE_VIDEO, 0x80A, METHOD_NEITHER, FILE_ANY_ACCESS )

typedef struct _DXGK_SESSION_USAGE
{
    ULONG Enable;
    ULONG Succeeded;
} DXGK_SESSION_USAGE, *PDXGK_SESSION_USAGE;

/*
 * watchdog's SMgrGdiCallout does not hand win32k the caller's Param as is. For the length of the
 * call it points Param at one of these, which carries the original value and the caller's display
 * scenario context (Reference watchdog.c:4149, SMgrGdiCalloutInternal). The callout puts Param back.
 */
typedef struct _SMGR_GDI_CALLOUT_PARAM
{
    ULONG_PTR Param;
    PVOID ScenarioContext;
} SMGR_GDI_CALLOUT_PARAM, *PSMGR_GDI_CALLOUT_PARAM;

/* Exported by watchdog.sys */
NTSTATUS
NTAPI
SMgrGdiCallout(
    _In_ PVOID Params,
    _In_ BOOLEAN AllSessions,
    _In_ BOOLEAN Synchronous,
    _In_opt_ PVOID Filter,
    _In_opt_ PVOID FilterContext,
    _In_opt_ PVOID ScenarioContext);

NTSTATUS
NTAPI
SMgrRegisterGdiCallout(
    _In_ PVOID Callout);

/*
 * The pointer index of each D3DKMT entry point in the DXGKWIN32K interface. This is how
 * win32kbase addresses them, one global per slot, rather than by a named field.
 */
#define DXGK_SLOT_CheckExclusiveOwnership              67
#define DXGK_SLOT_CheckMonitorPowerState               66
#define DXGK_SLOT_CheckOcclusion                       64
#define DXGK_SLOT_CloseAdapter                         12
#define DXGK_SLOT_CreateAllocation                     13
#define DXGK_SLOT_CreateContext                        26
#define DXGK_SLOT_CreateDevice                         24
#define DXGK_SLOT_CreateOverlay                        45
#define DXGK_SLOT_CreateSynchronizationObject          28
#define DXGK_SLOT_DestroyAllocation                    21
#define DXGK_SLOT_DestroyContext                       27
#define DXGK_SLOT_DestroyDevice                        25
#define DXGK_SLOT_DestroyOverlay                       48
#define DXGK_SLOT_DestroySynchronizationObject         30
#define DXGK_SLOT_Escape                               41
#define DXGK_SLOT_FlipOverlay                          47
#define DXGK_SLOT_GetContextSchedulingPriority         56
#define DXGK_SLOT_GetDeviceState                       54
#define DXGK_SLOT_GetDisplayModeList                   36
#define DXGK_SLOT_GetMultisampleMethodList             38
#define DXGK_SLOT_GetPresentHistory                    51
#define DXGK_SLOT_GetProcessSchedulingPriorityClass    58
#define DXGK_SLOT_GetRuntimeData                       39
#define DXGK_SLOT_GetScanLine                          60
#define DXGK_SLOT_GetSharedPrimaryHandle               44
#define DXGK_SLOT_InvalidateActiveVidPn                63
#define DXGK_SLOT_Lock                                 33
#define DXGK_SLOT_OpenResource                         19
#define DXGK_SLOT_PollDisplayChildren                  62
#define DXGK_SLOT_Present                              43
#define DXGK_SLOT_QueryAdapterInfo                     40
#define DXGK_SLOT_QueryAllocationResidency             23
#define DXGK_SLOT_QueryResourceInfo                    14
#define DXGK_SLOT_QueryStatistics                      42
#define DXGK_SLOT_ReleaseProcessVidPnSourceOwners      59
#define DXGK_SLOT_Render                               35
#define DXGK_SLOT_SetAllocationPriority                22
#define DXGK_SLOT_SetContextSchedulingPriority         55
#define DXGK_SLOT_SetDisplayMode                       37
#define DXGK_SLOT_SetDisplayPrivateDriverFormat        68
#define DXGK_SLOT_SetGammaRamp                         53
#define DXGK_SLOT_SetProcessSchedulingPriorityClass    57
#define DXGK_SLOT_SetQueuedLimit                       61
#define DXGK_SLOT_SetVidPnSourceOwner                  49
#define DXGK_SLOT_SignalSynchronizationObject          32
#define DXGK_SLOT_Unlock                               34
#define DXGK_SLOT_UpdateOverlay                        46
#define DXGK_SLOT_WaitForIdle                          65
#define DXGK_SLOT_WaitForSynchronizationObject         31
#define DXGK_SLOT_WaitForVerticalBlankEvent            52

#define DXGK_SLOT_CheckSharedResourceAccess            97
#define DXGK_SLOT_AdjustFullscreenGamma                213
#define DXGK_SLOT_CheckMultiPlaneOverlaySupport3       226
#define DXGK_SLOT_PresentMultiPlaneOverlay3            227
#define DXGK_SLOT_QueryVidPnExclusiveOwnership         206
#define DXGK_SLOT_SetVidPnSourceHwProtection           217
#define DXGK_SLOT_OpenAdapterFromLuid                  11
#define DXGK_SLOT_EnumAdapters2                        9
#define DXGK_SLOT_SetVidPnSourceOwner1                 50
/* Every one of them takes a single pointer, so one shape covers the whole table. */
typedef NTSTATUS (NTAPI *PFN_DXGK_D3DKMT)(PVOID);

/* Returns the entry point dxgkrnl put in that slot, or NULL when it filled none. */
PFN_DXGK_D3DKMT NTAPI DxgkGetD3DKMTSlot(_In_ ULONG Slot);
