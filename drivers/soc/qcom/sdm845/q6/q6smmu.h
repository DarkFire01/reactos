/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Apps SMMU state for the ADSP audio stream
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Logs the SMMU routing, then routes StreamId untranslated when Route is set and it has no entry.
   Returns whether the stream is routed. */
BOOLEAN
NTAPI
QcQ6SmmuRouteStream(
    _In_ ULONGLONG Base,
    _In_ USHORT StreamId,
    _In_ USHORT Mask,
    _In_ BOOLEAN Route);
