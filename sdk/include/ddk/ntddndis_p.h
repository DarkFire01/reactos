/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the NDIS device interface
 */

#pragma once

#include <ndis.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How far a selectively suspended adapter is allowed to go idle. The class
 * extension maps it onto the wake sources the client driver armed.
 */
typedef enum _NDIS_IDLE_CONDITION
{
    NdisIdleConditionAnyLowLatency = 0,
    NdisIdleConditionAny,
    NdisIdleConditionUnicastOnly,
    NdisIdleConditionL2ConnectedOnly
} NDIS_IDLE_CONDITION, *PNDIS_IDLE_CONDITION;

#ifdef __cplusplus
}
#endif
