/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Q6 circuit switched voice call path exports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Board AFE ports for the call, AFE port ids or MAXUSHORT when unset */
VOID
NTAPI
QcQ6VoiceInit(
    _In_ USHORT RxPort,
    _In_ USHORT TxPort);

/* Brings up the modem voice session on the dsp toward the board ports */
NTSTATUS
NTAPI
QcQ6VoiceStart(VOID);

VOID
NTAPI
QcQ6VoiceStop(VOID);

/* Uplink stream mute; kept for the next call when none is running */
NTSTATUS
NTAPI
QcQ6VoiceSetTxMute(
    _In_ BOOLEAN Mute,
    _In_ ULONG RampMs);

/* Api version the dsp core reports for one of its static services */
NTSTATUS
NTAPI
QcQ6CoreServiceVersion(
    _In_ ULONG ServiceId,
    _Out_ PULONG ApiVersion);
