/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Apps SMMU state for the ADSP audio stream
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Logs the stream entries and context banks in use and whether StreamId is routed */
VOID
NTAPI
QcQ6SmmuSurvey(
    _In_ ULONGLONG Base,
    _In_ USHORT StreamId);
