/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Header file for RDDM Undocumented shared info
 * COPYRIGHT:   Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Create an IO request to fill out the function pointer list */
#define IOCTL_VIDEO_DDI_FUNC_REGISTER \
	CTL_CODE( FILE_DEVICE_VIDEO, 0xF, METHOD_NEITHER, FILE_ANY_ACCESS  )

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

