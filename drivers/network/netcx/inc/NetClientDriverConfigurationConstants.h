/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Registry tunable names and identifiers
 *
 * The 26100 binary carries only the three DMA and verifier knobs, so the
 * imported sources are from a branch with more of them. Those three names are
 * taken from the binary; the rest follow the same PascalCase of the
 * identifier. A wrong name means the knob never reads from the registry and
 * keeps its default, which is why they are grouped rather than guessed at
 * individually.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _DRIVER_CONFIG_ENUM
{
    TX_THREAD_PRIORITY,
    RX_THREAD_PRIORITY,
    TX_THREAD_AFFINITY,
    RX_THREAD_AFFINITY,
    TX_THREAD_AFFINITY_ENABLED,
    RX_THREAD_AFFINITY_ENABLED,
    TX_REPORT_PERF_COUNTERS,
    TX_PERF_COUNTERS_ITERATION_INTERVAL,
    RX_REPORT_PERF_COUNTERS,
    RX_PERF_COUNTERS_ITERATION_INTERVAL,
    EC_UPDATE_PERF_COUNTERS,
    ALLOW_DMA_HAL_BYPASS,
    DMA_BOUNCE_POLICY,
    IGNORE_VERIFIER_DEBUG_BREAK,
    DRIVER_CONFIG_ENUM_MAX
} DRIVER_CONFIG_ENUM;

/* Confirmed against the binary. */
#define ALLOW_DMA_HAL_BYPASS_NAME                   L"AllowDmaHalBypass"
#define DMA_BOUNCE_POLICY_NAME                      L"DmaBouncePolicy"
#define IGNORE_VERIFIER_DEBUG_BREAK_NAME            L"IgnoreVerifierDebugBreak"

/* Derived from the same naming pattern. */
#define TX_THREAD_PRIORITY_NAME                     L"TxThreadPriority"
#define RX_THREAD_PRIORITY_NAME                     L"RxThreadPriority"
#define TX_THREAD_AFFINITY_NAME                     L"TxThreadAffinity"
#define RX_THREAD_AFFINITY_NAME                     L"RxThreadAffinity"
#define TX_THREAD_AFFINITY_ENABLED_NAME             L"TxThreadAffinityEnabled"
#define RX_THREAD_AFFINITY_ENABLED_NAME             L"RxThreadAffinityEnabled"
#define TX_REPORT_PERF_COUNTERS_NAME                L"TxReportPerfCounters"
#define TX_PERF_COUNTERS_ITERATION_INTERVAL_NAME    L"TxPerfCountersIterationInterval"
#define RX_REPORT_PERF_COUNTERS_NAME                L"RxReportPerfCounters"
#define RX_PERF_COUNTERS_ITERATION_INTERVAL_NAME    L"RxPerfCountersIterationInterval"
#define EC_UPDATE_PERF_COUNTERS_NAME                L"EcUpdatePerfCounters"

#define DRIVER_CONFIG_KNOB_IS_BOOLEAN               0x00000001

#define THREAD_AFFINITY_NO_MASK                     MAXULONG

#ifdef __cplusplus
}
#endif
