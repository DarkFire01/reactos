/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Q6 playback graph exports consumed by the wave miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Called at DISPATCH_LEVEL each time the dsp finishes rendering one period */
typedef VOID
(NTAPI *PQC_Q6_PERIOD_DONE)(
    _In_opt_ PVOID Context,
    _In_ ULONG Period);

/* Lines are the AFE I2S data line masks the board wires to each quaternary MI2S port */
VOID
NTAPI
QcQ6GraphInit(
    _In_ ULONG RxLine,
    _In_ ULONG TxLine);

/* Starts a backend AFE port for another client, shared with playback by count */
NTSTATUS
NTAPI
QcQ6GraphLinkOpen(
    _In_ USHORT PortId);

VOID
NTAPI
QcQ6GraphLinkClose(
    _In_ USHORT PortId);

/* Whether the ADSP may be handed host buffer addresses, which needs its SMMU stream routed.
   AddressTag is ORed into every address so the ADSP issues the access on that stream. */
VOID
NTAPI
QcQ6GraphAllowHostBuffers(
    _In_ BOOLEAN Allow,
    _In_ ULONGLONG AddressTag);

/* Brings up the AFE/ADM/ASM playback graph over the shared pcm buffer */
NTSTATUS
NTAPI
QcQ6PlaybackStart(
    _In_ ULONGLONG BufferPhys,
    _In_ ULONG BufferBytes,
    _In_ ULONG PeriodBytes,
    _In_ ULONG Rate,
    _In_ ULONG Channels,
    _In_ ULONG Bits,
    _In_opt_ PQC_Q6_PERIOD_DONE PeriodDone,
    _In_opt_ PVOID Context);

VOID
NTAPI
QcQ6PlaybackStop(VOID);

/* Total bytes the dsp has rendered since the stream started */
ULONGLONG
NTAPI
QcQ6PlaybackPosition(VOID);
