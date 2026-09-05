/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kdnet initialization entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _KDNET_INIT_H_
#define _KDNET_INIT_H_

#include <reactos/kdnetextensibility.h>

NTSTATUS NTAPI KdDebuggerInitialize0(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock);
NTSTATUS NTAPI KdDebuggerInitialize1(_In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock);

#endif /* _KDNET_INIT_H_ */
