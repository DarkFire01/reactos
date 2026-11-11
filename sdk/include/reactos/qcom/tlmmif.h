/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Top level mode multiplexer (TLMM) pin interface (qcomtlmm.sys)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

typedef struct _QCOM_TLMM *PQCOM_TLMM;

typedef enum _QCOM_TLMM_PULL
{
    TlmmPullNone = 0,
    TlmmPullDown = 1,
    TlmmPullKeeper = 2,
    TlmmPullUp = 3
} QCOM_TLMM_PULL;

typedef enum _QCOM_TLMM_DIRECTION
{
    TlmmInput,
    TlmmOutputLow,
    TlmmOutputHigh
} QCOM_TLMM_DIRECTION;

/* Function 0 is plain GPIO, the others are the SoC specific alternates */
typedef struct _QCOM_TLMM_PIN
{
    ULONG Number;
    ULONG TileOffset;
    ULONG Function;
    QCOM_TLMM_PULL Pull;
    ULONG DriveMilliamps;
    QCOM_TLMM_DIRECTION Direction;
} QCOM_TLMM_PIN, *PQCOM_TLMM_PIN;

typedef const QCOM_TLMM_PIN *PCQCOM_TLMM_PIN;

NTSTATUS
NTAPI
QcTlmmConnect(
    _In_ PHYSICAL_ADDRESS Base,
    _In_ SIZE_T Length,
    _Out_ PQCOM_TLMM *Tlmm);

VOID
NTAPI
QcTlmmDisconnect(
    _In_ PQCOM_TLMM Tlmm);

NTSTATUS
NTAPI
QcTlmmConfigurePin(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin);

VOID
NTAPI
QcTlmmSetLevel(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin,
    _In_ BOOLEAN High);

BOOLEAN
NTAPI
QcTlmmGetLevel(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin);

VOID
NTAPI
QcTlmmDumpPin(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin,
    _In_z_ PCSTR Label);

/* How a pin's interrupt latches */
typedef enum _QCOM_TLMM_TRIGGER
{
    TlmmTriggerLevelLow,
    TlmmTriggerLevelHigh,
    TlmmTriggerRisingEdge,
    TlmmTriggerFallingEdge
} QCOM_TLMM_TRIGGER;

/*
 * Routes a pin's interrupt to the applications processor summary line and
 * clears any stale latch. The caller owns the summary GIC interrupt and demuxes
 * with QcTlmmInterruptActive and QcTlmmClearInterrupt, both of which touch only
 * the one pin and take no lock, so they are safe from an interrupt service.
 */
NTSTATUS
NTAPI
QcTlmmConfigureInterrupt(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin,
    _In_ QCOM_TLMM_TRIGGER Trigger);

BOOLEAN
NTAPI
QcTlmmInterruptActive(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin);

VOID
NTAPI
QcTlmmClearInterrupt(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin);
