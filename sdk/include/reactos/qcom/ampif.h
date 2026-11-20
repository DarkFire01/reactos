/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Speaker amplifier control exported by qcomamp.sys
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Muxes the speaker link pins and configures the amp, once, at PASSIVE_LEVEL */
NTSTATUS
NTAPI
QcAmpPrepare(VOID);

/* Enables the amp; call after the I2S port is clocking */
NTSTATUS
NTAPI
QcAmpStart(
    _In_ ULONG Rate,
    _In_ ULONG Bits,
    _In_ ULONG Channels);

VOID
NTAPI
QcAmpStop(VOID);

/* Level in 1/65536 dB, 0 is the loudest step, clamped at -14 dB */
VOID
NTAPI
QcAmpSetVolume(
    _In_ LONG Level);

VOID
NTAPI
QcAmpSetMute(
    _In_ BOOLEAN Mute);
