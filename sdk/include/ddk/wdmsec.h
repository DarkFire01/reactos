/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Default security descriptors for device objects
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The strings are defined in the wdmsec static library. The WDK header also
 * redirects a few I/O routines to Wdmlib versions this library does not have,
 * so those names are left pointing at the kernel.
 */

#ifndef _WDMSEC_H_
#define _WDMSEC_H_

#ifdef __cplusplus
extern "C" {
#endif

/* D:P, kernel and system only. */
extern const UNICODE_STRING SDDL_DEVOBJ_KERNEL_ONLY;
#define SDDL_DEVOBJ_INF_SUPPLIED SDDL_DEVOBJ_KERNEL_ONLY

extern const UNICODE_STRING SDDL_DEVOBJ_SYS_ALL;
extern const UNICODE_STRING SDDL_DEVOBJ_SYS_ALL_ADM_ALL;
extern const UNICODE_STRING SDDL_DEVOBJ_SYS_ALL_ADM_RX;
extern const UNICODE_STRING SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_R;
extern const UNICODE_STRING SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_R_RES_R;
extern const UNICODE_STRING SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R;
extern const UNICODE_STRING SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX;

#ifdef __cplusplus
}
#endif

#endif /* _WDMSEC_H_ */
